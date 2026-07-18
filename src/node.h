#pragma once

#include <memory>
#include <string>

#include "coordinator.h"
#include "gossip/gossip_thread.h"
#include "metrics.h"
#include "node_info.h"
#include "replica_client.h"
#include "storage/storage_engine.h"

using namespace std;

class Router;

// A single cluster member. Owns its shard (behind StorageEngine), its metrics,
// and a Coordinator that runs the quorum/versioning/read-repair logic. Node
// itself is the protocol edge: it parses a framed request payload, dispatches to
// the Coordinator (or forwards to the primary owner), and writes a framed
// response. The TCP accept loop lives in TCPServer, which calls handleRequest —
// this class has no networking loop of its own.
class Node {
   public:
    // Storage and Metrics are injected: RocksDB + PrometheusMetrics in production,
    // in-memory versions for a memory-only build/tests. Passing a null Metrics
    // selects InMemoryMetrics, so existing call sites (and tests) keep working
    // without a Prometheus dependency. `replicas` is likewise injectable — null
    // falls back to the one-shot TcpReplicaClient; main injects a pooled client
    // (Tier 4.3) so the coordinator's fan-out reuses connections.
    Node(const NodeInfo &info, Router *router, unique_ptr<StorageEngine> storage,
         unique_ptr<Metrics> metrics = nullptr, QuorumConfig cfg = {},
         unique_ptr<ReplicaClient> replicas = nullptr);

    // Handle one framed request payload; write a framed response to client_fd.
    void handleRequest(const string &payload, int client_fd);

    // Set the gossip thread (called from main after constructing everything).
    void setGossipThread(gossip::GossipThread *gossip) { gossip_ = gossip; }

    // Inject hint store + liveness check for sloppy quorum (called from main
    // once gossip is ready).
    void setHintStore(HintStore *store) { coordinator_->setHintStore(store); }
    void setLivenessCheck(Coordinator::IsAliveFn fn) {
        coordinator_->setLivenessCheck(std::move(fn));
    }

    // Access to the underlying storage (needed by anti-entropy thread to iterate keys).
    StorageEngine *storage() const { return storage_.get(); }

    NodeInfo info;

   private:
    // Op handlers. Each writes a framed response to client_fd.
    void handlePut(const vector<string> &f, const string &rawPayload, int client_fd);
    void handleGet(const vector<string> &f, int client_fd);
    void handleDelete(const vector<string> &f, const string &rawPayload, int client_fd);
    void handleReplicate(const vector<string> &f, int client_fd);    // coordinator→replica write
    void handleReadReplica(const vector<string> &f, int client_fd);  // coordinator→replica read
    void handleRingQuery(int client_fd);  // read-only ring snapshot (does not mutate the ring)
    void handleLeave(const vector<string> &f, int client_fd);  // administrative permanent removal

    int effN(int requested) const { return requested > 0 ? requested : cfg_.N; }
    int effW(int requested) const { return requested > 0 ? requested : cfg_.W; }
    int effR(int requested) const { return requested > 0 ? requested : cfg_.R; }

    Router *router_;
    unique_ptr<StorageEngine> storage_;
    unique_ptr<Metrics> metrics_;
    unique_ptr<ReplicaClient> replicas_;
    unique_ptr<Coordinator> coordinator_;
    QuorumConfig cfg_;
    gossip::GossipThread *gossip_ = nullptr;
};
