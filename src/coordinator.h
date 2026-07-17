#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "metrics.h"
#include "node_info.h"
#include "replica_client.h"
#include "storage/storage_engine.h"
#include "vector_clock.h"
#include "versioned_value.h"

class Router;
class HintStore;

// Default quorum knobs (Dynamo's classic N=3, W=2, R=2 → W+R>N, so a read set
// and a write set always overlap in at least one replica: strong-ish
// consistency, still available under a single node loss).
struct QuorumConfig {
    int N = 3;
    int W = 2;
    int R = 2;
    std::chrono::milliseconds timeout{500};
    // Upper bound on vector-clock entries (Tier 4.5). A clock gains an entry per
    // node that ever coordinates the key, so at thousands of nodes an unbounded
    // clock would bloat every value, message, and comparison. Generous on purpose:
    // pruning discards causal information, so we only do it well past the point a
    // real key needs (see docs/decisions/tier-4.5.md).
    std::size_t max_clock_entries = 20;
};

struct PutResult {
    bool ok = false;
    VectorClock clock;  // the new clock, returned so the client can use it as context
    std::string error;  // set when !ok, e.g. "quorum_not_met"
};

struct GetResult {
    enum class Status { OK, SIBLINGS, NOTFOUND, ERROR };
    Status status = Status::ERROR;
    std::vector<VersionedValue> values;  // 1 for OK, >1 for SIBLINGS, 0 otherwise
    std::string error;
};

// The request coordinator: quorum writes, quorum reads, clock reconciliation, and
// read repair. Owns no sockets — it reaches peers only through ReplicaClient and
// its own node's StorageEngine — which is why the whole thing is unit-testable
// with a fake client and an in-memory store.
class Coordinator {
   public:
    // is_alive_fn: optional callback that checks if a peer is alive (from SWIM).
    // When set, the write path uses sloppy quorum: writes intended for dead nodes
    // are routed to a stand-in, which stores a hint for later delivery.
    using IsAliveFn = std::function<bool(const std::string &node_id)>;

    Coordinator(NodeInfo self, Router *router, StorageEngine *storage, ReplicaClient *replicas,
                Metrics *metrics, QuorumConfig defaults = {});

    // Blocks until every background fan-out / read-repair thread this coordinator
    // launched has finished. This guarantees no detached worker outlives the
    // coordinator or the dependencies it borrows (Router/StorageEngine/
    // ReplicaClient) — in production the coordinator lives for the whole process
    // so this only matters at shutdown; in tests it makes teardown deterministic.
    ~Coordinator();

    Coordinator(const Coordinator &) = delete;
    Coordinator &operator=(const Coordinator &) = delete;

    // Write path. `context` is the clock the client last read (empty for a blind
    // write). Returns OK once W replicas (local write counts as one) acknowledge
    // within the deadline; otherwise quorum_not_met — a retryable status.
    PutResult coordinatePut(const std::string &key, const std::string &value,
                            const VectorClock &context, int N, int W);

    // Delete path. Writes a versioned *tombstone* by the same W-quorum as a put,
    // so the delete propagates and converges instead of being a local erase that
    // a stale replica could resurrect. Returns the tombstone's clock on success.
    PutResult coordinateDelete(const std::string &key, const VectorClock &context, int N, int W);

    // Read path. Gathers R responses, reconciles by vector clock, and returns the
    // dominant version (repairing stale replicas asynchronously) or the full
    // sibling set when versions are concurrent. A dominant *tombstone* reads back
    // as NOTFOUND (but still repairs stale replicas so the delete converges).
    GetResult coordinateGet(const std::string &key, int N, int R);

    const QuorumConfig &defaults() const { return defaults_; }

    // Inject the hint store and liveness check after construction (set from main
    // once the gossip layer is ready; the coordinator works without them — it just
    // can't do sloppy quorum).
    void setHintStore(HintStore *store) { hint_store_ = store; }
    void setLivenessCheck(IsAliveFn fn) { is_alive_fn_ = std::move(fn); }

   private:
    // Reads this node's own stored clock for a key (empty if absent). Used so a
    // blind write is bumped above what we already hold and can never be born
    // already-dominated.
    VectorClock localClock(const std::string &key) const;

    // Builds the clock a new write/delete should carry: the client's context with
    // our own entry bumped above max(context, local). Shared by put and delete.
    VectorClock bumpedClock(const std::string &key, const VectorClock &context) const;

    // The shared W-quorum write fan-out used by both put and delete: fans the
    // already-built VersionedValue out to the N owners and returns OK once W
    // acknowledge within the deadline, else quorum_not_met. `op` only selects the
    // metric label (put vs delete) — the fan-out logic is identical.
    PutResult writeQuorum(const std::string &key, const VersionedValue &vv, int N, int W, Op op);

    // Fire-and-forget: push `value` to `peer` (or to local storage if peer is
    // self) on a background thread, bumping the read-repair metric. Never blocks
    // the caller — read repair must add zero latency to the read it rides on.
    void repairAsync(const NodeInfo &peer, const std::string &key, const VersionedValue &value);

    // Runs `fn` on a detached thread, registered with the background tracker so
    // the destructor can wait for it. `fn` must capture everything it needs by
    // value (never `this`), so it stays valid even past this call's return.
    void spawnBackground(std::function<void()> fn);

    NodeInfo self_;
    Router *router_;
    StorageEngine *storage_;
    ReplicaClient *replicas_;
    Metrics *metrics_;
    QuorumConfig defaults_;

    // Hinted handoff support (optional; null until gossip layer is ready).
    HintStore *hint_store_ = nullptr;
    IsAliveFn is_alive_fn_;

    // Counts outstanding background threads; the destructor drains it to zero.
    struct Background;
    std::shared_ptr<Background> bg_;
};
