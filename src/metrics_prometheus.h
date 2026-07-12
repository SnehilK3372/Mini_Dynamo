#pragma once

// Prometheus-backed Metrics. Compiled ONLY into the kvstore node binary (gated by
// HAVE_PROMETHEUS in CMake), never into kv_core or the unit tests — so the
// coordinator/router/versioning code and GoogleTest stay free of any Prometheus
// or HTTP dependency. This owns a civetweb-backed Exposer that serves the
// scrape endpoint at http://<bind>/metrics for the lifetime of the node.

#include <array>
#include <memory>
#include <string>

#include "metrics.h"

namespace prometheus {
class Registry;
class Exposer;
class Counter;
class Histogram;
class Gauge;
}  // namespace prometheus

class PrometheusMetrics : public Metrics {
   public:
    // bind_address is host:port for the scrape server (e.g. "0.0.0.0:9101").
    // node_id is attached as a constant label so Grafana can slice by node even
    // before Prometheus adds its own `instance` label.
    PrometheusMetrics(const std::string &bind_address, const std::string &node_id);
    ~PrometheusMetrics() override;

    void incRequest(Op op) override;
    void observeLatency(Op op, double seconds) override;
    void incQuorumSuccess(Op op) override;
    void incQuorumFailure(Op op) override;
    void incReadRepair() override;
    uint64_t readRepairCount() const override;

    void incHintStored() override;
    void incHintDelivered() override;
    uint64_t hintStoredCount() const override;
    uint64_t hintDeliveredCount() const override;

    void incAntiEntropySync() override;
    void incAntiEntropyKeysRepaired() override;
    uint64_t antiEntropySyncCount() const override;
    uint64_t antiEntropyKeysRepairedCount() const override;

   private:
    // Registry must outlive the Exposer (which holds a weak_ptr to it), so it is
    // declared first and the Exposer is torn down before it.
    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer> exposer_;

    // One pre-created metric per Op (indexed by static_cast<int>(op)), so the hot
    // path never does a label-map lookup — it just dereferences a stored pointer.
    std::array<prometheus::Counter *, 3> requests_{};
    std::array<prometheus::Histogram *, 3> latency_{};
    std::array<prometheus::Counter *, 3> quorum_ok_{};
    std::array<prometheus::Counter *, 3> quorum_fail_{};
    prometheus::Counter *read_repair_ = nullptr;
    prometheus::Counter *hints_stored_ = nullptr;
    prometheus::Counter *hints_delivered_ = nullptr;
    prometheus::Counter *ae_syncs_ = nullptr;
    prometheus::Counter *ae_keys_repaired_ = nullptr;
    prometheus::Gauge *node_up_ = nullptr;
};
