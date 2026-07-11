#include "coordinator.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

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

PutResult Coordinator::coordinatePut(const string &key, const string &value,
                                     const VectorClock &context, int N, int W) {
    auto owners = router_->findOwners(key, N);
    if (owners.empty()) return {false, {}, "no_nodes_in_ring"};

    // Build the new clock. Base each entry on the client's context, but bump our
    // own entry strictly above whatever we already hold locally, so that even a
    // blind write (empty context) dominates our current version instead of being
    // born already-dominated and silently lost.
    VectorClock newClock = context;
    uint64_t base = max(context.get(self_.node_id), localClock(key).get(self_.node_id));
    newClock.set(self_.node_id, base + 1);

    VersionedValue vv{value, newClock};
    const string serialized = vv.serialize();

    auto q = make_shared<WriteQuorum>();
    int localAcks = 0;
    vector<NodeInfo> remotes;
    for (const auto &owner : owners) {
        if (owner.node_id == self_.node_id) {
            storage_->put(key, serialized);  // local write is synchronous and durable
            ++localAcks;
        } else {
            remotes.push_back(owner);
        }
    }
    {
        lock_guard<mutex> lk(q->m);
        q->acks = localAcks;
    }
    const int remoteCount = static_cast<int>(remotes.size());

    // Capture the borrowed pointer and timeout by value; the worker must never
    // touch `this`, which may be gone by the time a slow worker runs.
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

    // Wake as soon as W is met, or once every remote has answered (so a cluster
    // of instantly-failing replicas doesn't force us to burn the whole timeout).
    const auto deadline = chrono::steady_clock::now() + defaults_.timeout;
    int finalAcks;
    {
        unique_lock<mutex> lk(q->m);
        q->cv.wait_until(lk, deadline, [&] {
            return q->acks >= W || q->completed == remoteCount;
        });
        finalAcks = q->acks;
    }

    if (finalAcks >= W) return {true, newClock, ""};
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
    if (owners.empty()) return {GetResult::Status::ERROR, {}, "no_nodes_in_ring"};

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
        return {GetResult::Status::ERROR, {}, "quorum_not_met"};
    }

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
        bool stale = !r.found ||
                     VectorClock::compare(dominant.clock, r.vv.clock) == Ordering::A_DOMINATES;
        if (stale) repairAsync(r.peer, key, dominant);
    }
    return {GetResult::Status::OK, {dominant}, ""};
}
