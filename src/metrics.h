#pragma once

#include <atomic>
#include <cstdint>

// Minimal metrics seam. Tier 1A only needs a read-repair counter, but the build
// plan asks that it be exposed *through an interface* now so Tier 1C can back it
// with prometheus-cpp without touching coordinator code. Keep it tiny; grow it
// (request counts, latency histograms) when the observability tier lands.
class Metrics {
public:
    virtual ~Metrics() = default;

    // Called once per replica that a read repair pushes a fresh version to.
    virtual void incReadRepair() = 0;
    virtual uint64_t readRepairCount() const = 0;
};

// Default process-local implementation. Coordinator calls incReadRepair() from
// the read path (and, in the future, background threads), so the counter is
// atomic. relaxed ordering is fine: it is a monotonic statistic, not a
// synchronization signal.
class InMemoryMetrics : public Metrics {
public:
    void incReadRepair() override {
        read_repair_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t readRepairCount() const override {
        return read_repair_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> read_repair_{0};
};
