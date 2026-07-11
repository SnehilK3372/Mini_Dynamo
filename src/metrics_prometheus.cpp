#include "metrics_prometheus.h"

#include <map>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

using prometheus::BuildCounter;
using prometheus::BuildGauge;
using prometheus::BuildHistogram;

namespace {
// Sub-second-oriented buckets: cluster ops are expected in the single-digit-ms to
// low-hundreds-of-ms range, with headroom to a few seconds for a request that
// rides a quorum timeout. Prometheus derives p50/p95/p99 from these via
// histogram_quantile().
const prometheus::Histogram::BucketBoundaries kLatencyBuckets = {
    0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0};

int idx(Op op) { return static_cast<int>(op); }
}  // namespace

PrometheusMetrics::PrometheusMetrics(const std::string &bind_address, const std::string &node_id)
    : registry_(std::make_shared<prometheus::Registry>()) {
    // Constant label so every series is attributable to a node regardless of how
    // Prometheus labels the scrape target.
    const std::map<std::string, std::string> node = {{"node_id", node_id}};

    auto &requests_family = BuildCounter()
                                .Name("minidynamo_requests_total")
                                .Help("Client-facing requests handled, by op")
                                .Register(*registry_);
    auto &latency_family = BuildHistogram()
                               .Name("minidynamo_request_latency_seconds")
                               .Help("Request handling latency, by op")
                               .Register(*registry_);
    auto &quorum_family = BuildCounter()
                              .Name("minidynamo_quorum_total")
                              .Help("Quorum attempts by op and outcome (success|failure)")
                              .Register(*registry_);
    auto &repair_family = BuildCounter()
                              .Name("minidynamo_read_repair_total")
                              .Help("Replicas repaired to the dominant version after a read")
                              .Register(*registry_);
    auto &up_family = BuildGauge()
                          .Name("minidynamo_node_up")
                          .Help("1 while the node process is serving")
                          .Register(*registry_);

    for (Op op : {Op::Put, Op::Get, Op::Delete}) {
        std::map<std::string, std::string> l = node;
        l["op"] = opLabel(op);
        requests_[idx(op)] = &requests_family.Add(l);
        latency_[idx(op)] = &latency_family.Add(l, kLatencyBuckets);

        auto ok = l;
        ok["outcome"] = "success";
        quorum_ok_[idx(op)] = &quorum_family.Add(ok);
        auto fail = l;
        fail["outcome"] = "failure";
        quorum_fail_[idx(op)] = &quorum_family.Add(fail);
    }
    read_repair_ = &repair_family.Add(node);
    node_up_ = &up_family.Add(node);
    node_up_->Set(1);

    // Stand up the scrape server last, once every collectable is registered.
    exposer_ = std::make_unique<prometheus::Exposer>(bind_address);
    exposer_->RegisterCollectable(registry_);
}

// Defined here (not defaulted in the header) so the unique_ptr<Exposer> and
// shared_ptr<Registry> see prometheus' complete types at their destruction site.
PrometheusMetrics::~PrometheusMetrics() = default;

void PrometheusMetrics::incRequest(Op op) { requests_[idx(op)]->Increment(); }
void PrometheusMetrics::observeLatency(Op op, double seconds) {
    latency_[idx(op)]->Observe(seconds);
}
void PrometheusMetrics::incQuorumSuccess(Op op) { quorum_ok_[idx(op)]->Increment(); }
void PrometheusMetrics::incQuorumFailure(Op op) { quorum_fail_[idx(op)]->Increment(); }
void PrometheusMetrics::incReadRepair() { read_repair_->Increment(); }

uint64_t PrometheusMetrics::readRepairCount() const {
    return static_cast<uint64_t>(read_repair_->Value());
}
