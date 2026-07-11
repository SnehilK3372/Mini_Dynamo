#pragma once

#include <memory>
#include <string>

#include "coordinator.h"
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
    // Storage is injected (RocksDB in production, in-memory for a memory-only
    // build/tests). Node builds its own Metrics and TCP-backed ReplicaClient and
    // wires up the Coordinator over them.
    Node(const NodeInfo &info, Router *router, unique_ptr<StorageEngine> storage,
         QuorumConfig cfg = {});

    // Handle one framed request payload; write a framed response to client_fd.
    void handleRequest(const string &payload, int client_fd);

    NodeInfo info;

private:
    // Op handlers. Each writes a framed response to client_fd.
    void handlePut(const vector<string> &f, const string &rawPayload, int client_fd);
    void handleGet(const vector<string> &f, int client_fd);
    void handleDelete(const vector<string> &f, const string &rawPayload, int client_fd);
    void handleReplicate(const vector<string> &f, int client_fd);   // coordinator→replica write
    void handleReadReplica(const vector<string> &f, int client_fd); // coordinator→replica read
    void handleJoin(const string &payload, int client_fd);
    void handleRingQuery(int client_fd);  // read-only ring snapshot (does not mutate the ring)

    int effN(int requested) const { return requested > 0 ? requested : cfg_.N; }
    int effW(int requested) const { return requested > 0 ? requested : cfg_.W; }
    int effR(int requested) const { return requested > 0 ? requested : cfg_.R; }

    Router *router_;
    unique_ptr<StorageEngine> storage_;
    unique_ptr<Metrics> metrics_;
    unique_ptr<ReplicaClient> replicas_;
    unique_ptr<Coordinator> coordinator_;
    QuorumConfig cfg_;
};
