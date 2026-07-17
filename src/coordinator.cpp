#include "coordinator.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "hints/hint_store.h"
#include "router.h"

using namespace std;
using Ordering = VectorClock::Ordering;

// Tracks how many background (fan-out / read-repair) threads are still running so
// the destructor can wait for them. Held via shared_ptr and captured by each
// worker, so the counter itself outlives the coordinator if a worker somehow
// lingers — though the destructor's drain means that never actually happens.
struct Coordinator::Background {
    mutex m;
    condition_variable cv;
    int outstanding = 0;
};

Coordinator::Coordinator(NodeInfo self, Router *router, StorageEngine *storage,
                         ReplicaClient *replicas, Metrics *metrics, QuorumConfig defaults)
    : self_(move(self)),
      router_(router),
      storage_(storage),
      replicas_(replicas),
      metrics_(metrics),
      defaults_(defaults),
      bg_(make_shared<Background>()) {}

Coordinator::~Coordinator() {
    unique_lock<mutex> lk(bg_->m);
    bg_->cv.wait(lk, [&] { return bg_->outstanding == 0; });
}

void Coordinator::spawnBackground(function<void()> fn) {
    {
        lock_guard<mutex> lk(bg_->m);
        ++bg_->outstanding;
    }
    auto bg = bg_;  // keep the tracker alive for the worker
    thread([bg, fn = move(fn)]() {
        fn();
        lock_guard<mutex> lk(bg->m);
        --bg->outstanding;
        bg->cv.notify_all();
    }).detach();
}

VectorClock Coordinator::localClock(const string &key) const {
    auto stored = storage_->get(key);
    if (!stored) return {};
    return VersionedValue::deserialize(*stored).clock;
}

// ---------------------------------------------------------------------------
// PUT: quorum write
// ---------------------------------------------------------------------------

namespace {
// Shared accumulator for a fan-out. Held via shared_ptr so it outlives the
// coordinating call: a replica thread that finishes *after* the deadline still
// has a live object to write into, so nothing dangles and its late result is
// simply ignored. This is the reason we hand-roll threads + a condition_variable
// instead of std::async, whose abandoned-future destructor would *join* a slow
// replica and hang the coordinator past its own deadline.
struct WriteQuorum {
    mutex m;
    condition_variable cv;
    int acks = 0;
    int completed = 0;
};
}  // namespace

VectorClock Coordinator::bumpedClock(const string &key, const VectorClock &context) const {
    // Base each entry on the client's context, but bump our own entry strictly
    // above whatever we already hold locally, so that even a blind write (empty
    // context) dominates our current version instead of being born
    // already-dominated and silently lost.
    VectorClock newClock = context;
    uint64_t base = max(context.get(self_.node_id), localClock(key).get(self_.node_id));
    newClock.set(self_.node_id, base + 1);
    // Bound the clock before it is stored and replicated (Tier 4.5). Pruning
    // *after* our own bump is deliberate: set() just stamped our entry with the
    // current time, so it is the newest and can never be the entry dropped —
    // the write we are coordinating always survives in the clock it carries.
    newClock.prune(defaults_.max_clock_entries);
    return newClock;
}

PutResult Coordinator::coordinatePut(const string &key, const string &value,
                                     const VectorClock &context, int N, int W) {
    // A live write and a delete differ only in the payload they carry: a delete
    // is a tombstone VersionedValue. Everything else — clock bump, quorum
    // fan-out, never-regress replication — is identical, so both go through
    // writeQuorum.
    VersionedValue vv{value, bumpedClock(key, context), /*deleted=*/false};
    return writeQuorum(key, vv, N, W, Op::Put);
}

// A delete is a versioned tombstone written by quorum, not a local erase: it
// must dominate the value it removes and then converge onto the other replicas
// (via replication now, read repair later) — otherwise a surviving replica would
// resurrect the key on the next read.
PutResult Coordinator::coordinateDelete(const string &key, const VectorClock &context, int N,
                                        int W) {
    VersionedValue tombstone{/*data=*/"", bumpedClock(key, context), /*deleted=*/true};
    return writeQuorum(key, tombstone, N, W, Op::Delete);
}

PutResult Coordinator::writeQuorum(const string &key, const VersionedValue &vv, int N, int W,
                                   Op op) {
    auto owners = router_->findOwners(key, N);
    if (owners.empty()) {
        metrics_->incQuorumFailure(op);
        return {false, {}, "no_nodes_in_ring"};
    }

    const VectorClock &newClock = vv.clock;
    const string serialized = vv.serialize();

    auto q = make_shared<WriteQuorum>();
    int localAcks = 0;
    vector<NodeInfo> remotes;

    // Sloppy quorum: if a target is known-dead (via gossip), pick the next alive
    // node from the ring as a stand-in and store a hint for later delivery.
    // This keeps writes available through single-node failures.
    //
    // The candidate list is fetched LAZILY — at most once, and only if an owner is
    // actually dead. getAllPhysicalNodes() copies the whole membership under a
    // lock, so doing it eagerly charged every write O(cluster size) (a 1000-element
    // copy per write at 1000 nodes) to serve a branch that almost never runs.
    vector<NodeInfo> all_nodes;
    bool all_nodes_loaded = false;
    auto candidates = [&]() -> const vector<NodeInfo> & {
        if (!all_nodes_loaded) {
            all_nodes = router_->getAllPhysicalNodes();
            all_nodes_loaded = true;
        }
        return all_nodes;
    };

    for (const auto &owner : owners) {
        if (owner.node_id == self_.node_id) {
            storage_->put(key, serialized);
            ++localAcks;
        } else if (is_alive_fn_ && !is_alive_fn_(owner.node_id)) {
            // Owner is dead — find a stand-in from the ring that is alive and
            // not already in the owner list.
            bool found_standin = false;
            for (const auto &candidate : candidates()) {
                if (candidate.node_id == self_.node_id) continue;
                if (candidate.node_id == owner.node_id) continue;
                bool already_in_owners = false;
                for (const auto &o : owners) {
                    if (o.node_id == candidate.node_id) {
                        already_in_owners = true;
                        break;
                    }
                }
                if (already_in_owners) continue;
                if (!is_alive_fn_(candidate.node_id)) continue;
                // Use this candidate as stand-in.
                remotes.push_back(candidate);
                found_standin = true;
                break;
            }
            // Store the hint whether or not a stand-in was found: it is what lets
            // the write reach the real owner once it comes back.
            if (hint_store_) {
                hint_store_->store(owner.node_id, key, vv);
                metrics_->incHintStored();
            }
            // Deliberately no local write / no extra ack when there is no stand-in.
            // Doing that (as this used to) double-counted THIS node: if self is
            // already an owner it had stored the value and acked on the branch
            // above, so acking again let one physical copy satisfy two acks —
            // W=3 could "succeed" against only two real replicas. An ack must mean
            // a distinct replica holds the value. With no stand-in there is simply
            // one fewer replica, and W fails if that leaves it unmet — which is
            // the honest answer.
            (void)found_standin;
        } else {
            remotes.push_back(owner);
        }
    }
    {
        lock_guard<mutex> lk(q->m);
        q->acks = localAcks;
    }
    const int remoteCount = static_cast<int>(remotes.size());

    ReplicaClient *replicas = replicas_;
    auto timeout = defaults_.timeout;
    for (const auto &peer : remotes) {
        spawnBackground([replicas, q, peer, key, vv, timeout]() {
            ReplicaWriteResult r = replicas->writeReplica(peer, key, vv, timeout);
            lock_guard<mutex> lk(q->m);
            ++q->completed;
            if (r.ok) ++q->acks;
            q->cv.notify_all();
        });
    }

    const auto deadline = chrono::steady_clock::now() + defaults_.timeout;
    int finalAcks;
    {
        unique_lock<mutex> lk(q->m);
        q->cv.wait_until(lk, deadline, [&] { return q->acks >= W || q->completed == remoteCount; });
        finalAcks = q->acks;
    }

    if (finalAcks >= W) {
        metrics_->incQuorumSuccess(op);
        return {true, newClock, ""};
    }
    metrics_->incQuorumFailure(op);
    return {false, {}, "quorum_not_met"};
}

// ---------------------------------------------------------------------------
// GET: quorum read + reconciliation + read repair
// ---------------------------------------------------------------------------

namespace {
struct Resp {
    NodeInfo peer;
    bool found = false;
    VersionedValue vv;
};

struct ReadQuorum {
    mutex m;
    condition_variable cv;
    vector<Resp> responses;  // one per replica that answered (found or not)
    int completed = 0;       // remote threads that have finished (answered or failed)
};

// The maximal set: versions not strictly dominated by any other response,
// deduplicated so identical clocks collapse to one entry. Size 1 → a single
// current version. Size >1 → concurrent siblings.
vector<VersionedValue> maximalVersions(const vector<Resp> &responses) {
    vector<VersionedValue> found;
    for (const auto &r : responses) {
        if (r.found) found.push_back(r.vv);
    }
    vector<VersionedValue> maximal;
    for (const auto &cand : found) {
        bool dominated = false;
        for (const auto &other : found) {
            if (VectorClock::compare(other.clock, cand.clock) == Ordering::A_DOMINATES) {
                dominated = true;  // `other` strictly dominates `cand`
                break;
            }
        }
        if (dominated) continue;
        bool duplicate = false;
        for (const auto &m : maximal) {
            if (VectorClock::compare(m.clock, cand.clock) == Ordering::EQUAL) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) maximal.push_back(cand);
    }
    return maximal;
}
}  // namespace

void Coordinator::repairAsync(const NodeInfo &peer, const string &key,
                              const VersionedValue &value) {
    metrics_->incReadRepair();
    if (peer.node_id == self_.node_id) {
        // Local staleness: just overwrite our own copy. Cheap and synchronous
        // enough that it never delays the read meaningfully.
        storage_->put(key, value.serialize());
        return;
    }
    ReplicaClient *replicas = replicas_;
    NodeInfo target = peer;
    auto timeout = defaults_.timeout;
    spawnBackground([replicas, target, key, value, timeout]() {
        replicas->writeReplica(target, key, value, timeout);
    });
}

GetResult Coordinator::coordinateGet(const string &key, int N, int R) {
    auto owners = router_->findOwners(key, N);
    if (owners.empty()) {
        metrics_->incQuorumFailure(Op::Get);
        return {GetResult::Status::ERROR, {}, "no_nodes_in_ring"};
    }

    auto q = make_shared<ReadQuorum>();
    vector<NodeInfo> remotes;
    for (const auto &owner : owners) {
        if (owner.node_id == self_.node_id) {
            Resp r;
            r.peer = owner;
            auto stored = storage_->get(key);
            if (stored) {
                r.found = true;
                r.vv = VersionedValue::deserialize(*stored);
            }
            q->responses.push_back(r);  // local read always counts as a response
        } else if (is_alive_fn_ && !is_alive_fn_(owner.node_id)) {
            // Liveness-aware fan-out: a replica gossip has confirmed dead would
            // only ever time out, and each such read blocks a background thread
            // for the full quorum deadline — under load that thread/CPU pressure
            // is enough to push the *live* replicas past the deadline too, failing
            // reads that should have succeeded. Skip it. With N=3 and one dead
            // owner, the local read + the one live remote still meet R=2. If too
            // many owners are dead to reach R, the read fails fast below with
            // quorum_not_met, which is the honest outcome (better than stalling).
            continue;
        } else {
            remotes.push_back(owner);
        }
    }
    const int remoteCount = static_cast<int>(remotes.size());

    ReplicaClient *replicas = replicas_;
    auto timeout = defaults_.timeout;
    for (const auto &peer : remotes) {
        spawnBackground([replicas, q, peer, key, timeout]() {
            ReplicaReadResult rr = replicas->readReplica(peer, key, timeout);
            lock_guard<mutex> lk(q->m);
            ++q->completed;
            if (rr.ok) {  // a replica that failed to answer is not a response
                Resp r;
                r.peer = peer;
                r.found = rr.found;
                r.vv = rr.value;
                q->responses.push_back(r);
            }
            q->cv.notify_all();
        });
    }

    const auto deadline = chrono::steady_clock::now() + defaults_.timeout;
    vector<Resp> responses;
    {
        unique_lock<mutex> lk(q->m);
        q->cv.wait_until(lk, deadline, [&] {
            return static_cast<int>(q->responses.size()) >= R || q->completed == remoteCount;
        });
        responses = q->responses;  // snapshot; late responses after this are ignored
    }

    if (static_cast<int>(responses.size()) < R) {
        metrics_->incQuorumFailure(Op::Get);
        return {GetResult::Status::ERROR, {}, "quorum_not_met"};
    }
    // R responses gathered: the read quorum was met. Whether the key exists,
    // is a tombstone, or has siblings is a data outcome, not a quorum outcome.
    metrics_->incQuorumSuccess(Op::Get);

    auto maximal = maximalVersions(responses);
    if (maximal.empty()) {
        return {GetResult::Status::NOTFOUND, {}, ""};
    }
    if (maximal.size() > 1) {
        // Concurrent versions: hand the client every sibling to reconcile. We do
        // not repair here — writing back one sibling would masquerade as a
        // resolution the client never made. (Sibling write-back / hinted handoff
        // / anti-entropy are the named future work.)
        return {GetResult::Status::SIBLINGS, maximal, ""};
    }

    // Single dominant version: answer the client, then repair every replica that
    // was strictly behind (or missing the key). Repairs run on background
    // threads, so the read has already returned by the time they land.
    const VersionedValue &dominant = maximal.front();
    for (const auto &r : responses) {
        bool stale =
            !r.found || VectorClock::compare(dominant.clock, r.vv.clock) == Ordering::A_DOMINATES;
        if (stale) repairAsync(r.peer, key, dominant);
    }
    // A dominant tombstone means the key is deleted: repair still ran above so the
    // tombstone converges onto replicas that missed it (otherwise one of them
    // could resurrect the key), but the client sees NOTFOUND, not the empty body.
    if (dominant.deleted) {
        return {GetResult::Status::NOTFOUND, {}, ""};
    }
    return {GetResult::Status::OK, {dominant}, ""};
}
