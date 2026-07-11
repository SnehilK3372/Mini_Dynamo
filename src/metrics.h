#pragma once

#include <atomic>
#include <cstdint>

// Operation label shared by every op-keyed metric. Kept as a small enum (not a
// string) so hot-path call sites don't allocate and the Prometheus backend can
// pre-create one metric per op and select it with a switch instead of a
// per-request label lookup.
enum class Op { Put, Get, Delete };

inline const char *opLabel(Op op) {
    switch (op) {
        case Op::Put:    return "put";
        case Op::Get:    return "get";
        case Op::Delete: return "delete";
    }
    return "unknown";
}

// The metrics seam. Tier 1A introduced it with just a read-repair counter so the
// coordinator could record repairs through an interface; Tier 1C grows it to the
// full observable surface (request rate, latency, quorum outcomes) so
// prometheus-cpp can back it in the node binary *without* the coordinator or any
// unit test taking a dependency on Prometheus. `kv_core` and the tests link only
// against this header + InMemoryMetrics; the Prometheus implementation lives in
// the node-only translation unit metrics_prometheus.cpp.
class Metrics {
public:
    virtual ~Metrics() = default;

    // One client-facing request of the given op arrived at this node.
    virtual void incRequest(Op op) = 0;
    // Wall-clock seconds that request took (feeds a latency histogram).
    virtual void observeLatency(Op op, double seconds) = 0;
    // A quorum write/read for this op met (success) or missed (failure) its W/R.
    virtual void incQuorumSuccess(Op op) = 0;
    virtual void incQuorumFailure(Op op) = 0;

    // Called once per replica that a read repair pushes a fresh version to.
    virtual void incReadRepair() = 0;
    virtual uint64_t readRepairCount() const = 0;
};

// Default process-local implementation: plain atomic counters, no HTTP surface.
// Used by every unit test and by a node built without Prometheus. Counters use
// relaxed ordering because they are monotonic statistics, not synchronization
// signals. Latency is dropped here — histograms only matter once something is
// scraping them, which is the Prometheus backend's job.
class InMemoryMetrics : public Metrics {
public:
    void incRequest(Op) override { requests_.fetch_add(1, std::memory_order_relaxed); }
    void observeLatency(Op, double) override {}
    void incQuorumSuccess(Op) override { quorum_ok_.fetch_add(1, std::memory_order_relaxed); }
    void incQuorumFailure(Op) override { quorum_fail_.fetch_add(1, std::memory_order_relaxed); }

    void incReadRepair() override { read_repair_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t readRepairCount() const override {
        return read_repair_.load(std::memory_order_relaxed);
    }

    // Extra readers so tests can assert on the new counters if they want to;
    // the interface deliberately keeps only readRepairCount() virtual.
    uint64_t requestCount() const { return requests_.load(std::memory_order_relaxed); }
    uint64_t quorumSuccessCount() const { return quorum_ok_.load(std::memory_order_relaxed); }
    uint64_t quorumFailureCount() const { return quorum_fail_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> requests_{0};
    std::atomic<uint64_t> quorum_ok_{0};
    std::atomic<uint64_t> quorum_fail_{0};
    std::atomic<uint64_t> read_repair_{0};
};
