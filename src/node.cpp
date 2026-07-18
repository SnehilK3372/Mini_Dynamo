#include "node.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include "base64.h"
#include "log.h"
#include "net/framing.h"
#include "net/tcp_client.h"
#include "net/tcp_replica_client.h"
#include "replica_ops.h"
#include "router.h"

using namespace std;

namespace {
// Split on '|' preserving empty fields (a trailing empty clock must survive).
vector<string> splitAll(const string &s, char delim) {
    vector<string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(delim, start);
        if (p == string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

int toInt(const vector<string> &f, size_t i, int def = 0) {
    if (i >= f.size() || f[i].empty()) return def;
    try {
        return stoi(f[i]);
    } catch (...) {
        return def;
    }
}

string field(const vector<string> &f, size_t i) { return i < f.size() ? f[i] : string(); }

void reply(int fd, const string &payload) { framing::sendFramed(fd, payload); }
}  // namespace

Node::Node(const NodeInfo &info_, Router *router, unique_ptr<StorageEngine> storage,
           unique_ptr<Metrics> metrics, QuorumConfig cfg, unique_ptr<ReplicaClient> replicas)
    : info(info_),
      router_(router),
      storage_(move(storage)),
      metrics_(metrics ? move(metrics) : make_unique<InMemoryMetrics>()),
      replicas_(replicas ? move(replicas) : make_unique<TcpReplicaClient>(info_.node_id)),
      cfg_(cfg) {
    coordinator_ = make_unique<Coordinator>(info, router_, storage_.get(), replicas_.get(),
                                            metrics_.get(), cfg_);
}

void Node::handleRequest(const string &payload, int client_fd) {
    auto f = splitAll(payload, '|');
    if (f.empty() || f[0].empty()) return;
    const string &type = f[0];

    // Client-facing ops are timed and counted here at the protocol edge (the
    // internal REPLICATE/READ/RING verbs are cluster plumbing, not client
    // traffic, so they don't feed the request-rate/latency metrics). A forwarded
    // PUT/DELETE is counted on both the receiving node and the primary — each did
    // handle a request — which is the honest per-node view of load.
    if (type == "PUT" || type == "GET" || type == "DELETE") {
        const Op op = type == "PUT" ? Op::Put : (type == "GET" ? Op::Get : Op::Delete);
        metrics_->incRequest(op);
        const auto start = chrono::steady_clock::now();
        if (type == "PUT") {
            handlePut(f, payload, client_fd);
        } else if (type == "GET") {
            handleGet(f, client_fd);
        } else {
            handleDelete(f, payload, client_fd);
        }
        metrics_->observeLatency(
            op, chrono::duration<double>(chrono::steady_clock::now() - start).count());
    } else if (type == "REPLICATE") {
        handleReplicate(f, client_fd);
    } else if (type == "READ") {
        handleReadReplica(f, client_fd);
    } else if (type == "RING") {
        handleRingQuery(client_fd);
    } else if (type == "LEAVE") {
        handleLeave(f, client_fd);
    } else if (type == "SWIM_PING" || type == "SWIM_PING_REQ" || type == "SWIM_ACK" ||
               type == "SWIM_JOIN" || type == "SWIM_SYNC" || type == "SWIM_SYNC_ACK") {
        if (gossip_) {
            string resp = gossip_->handleMessage(payload);
            if (!resp.empty()) reply(client_fd, resp);
        }
    } else {
        reply(client_fd, "RESPONSE|ERROR|unknown_command");
    }
}

// PUT|<key>|<b64value>|<origin>|<N>|<W>|<R>|<clock>
void Node::handlePut(const vector<string> &f, const string &rawPayload, int client_fd) {
    const string key = field(f, 1);
    auto owners = router_->findOwners(key, 1);
    if (owners.empty()) {
        reply(client_fd, "RESPONSE|ERROR|no_nodes_in_ring");
        return;
    }
    const NodeInfo &primary = owners[0];

    // A write is coordinated by the key's primary owner (its deterministic
    // clock-incrementer). If we are not it, forward the request verbatim and
    // relay the reply.
    if (primary.node_id != info.node_id) {
        TCPClient client;
        string resp = client.sendAndReceiveFramed(primary.host, primary.port, rawPayload,
                                                  static_cast<int>(cfg_.timeout.count()) * 2);
        bool ok = !resp.empty();
        jlog::op(ok ? "info" : "warn", "put", key, ok ? "forwarded" : "forward_failed");
        reply(client_fd, ok ? resp : "RESPONSE|ERROR|forward_failed");
        return;
    }

    const string value = base64::decode(field(f, 2));
    VectorClock context = VectorClock::parse(field(f, 7));
    int N = effN(toInt(f, 4));
    int W = effW(toInt(f, 5));

    PutResult pr = coordinator_->coordinatePut(key, value, context, N, W);
    jlog::op(pr.ok ? "info" : "warn", "put", key, pr.ok ? "ok" : pr.error);
    if (pr.ok) {
        reply(client_fd, "RESPONSE|OK|" + pr.clock.serialize());
    } else {
        reply(client_fd, "RESPONSE|ERROR|" + pr.error);
    }
}

// GET|<key>|<origin>|<N>|<R>   (coordinated by whichever node receives it)
void Node::handleGet(const vector<string> &f, int client_fd) {
    const string key = field(f, 1);
    int N = effN(toInt(f, 3));
    int R = effR(toInt(f, 4));

    GetResult gr = coordinator_->coordinateGet(key, N, R);
    switch (gr.status) {
        case GetResult::Status::OK: {
            const auto &v = gr.values.front();
            jlog::op("info", "get", key, "ok");
            reply(client_fd, "RESPONSE|OK|" + base64::encode(v.data) + "|" + v.clock.serialize());
            break;
        }
        case GetResult::Status::SIBLINGS: {
            ostringstream oss;
            oss << "RESPONSE|SIBLINGS|" << gr.values.size();
            for (const auto &v : gr.values) {
                oss << "|" << base64::encode(v.data) << "|" << v.clock.serialize();
            }
            jlog::op("info", "get", key, "siblings");
            reply(client_fd, oss.str());
            break;
        }
        case GetResult::Status::NOTFOUND:
            jlog::op("info", "get", key, "notfound");
            reply(client_fd, "RESPONSE|NOTFOUND");
            break;
        case GetResult::Status::ERROR:
            jlog::op("warn", "get", key, gr.error);
            reply(client_fd, "RESPONSE|ERROR|" + gr.error);
            break;
    }
}

// DELETE|<key>|<origin>|<N>|<W>|<clock>
// A delete is coordinated by the key's primary owner exactly like a PUT (it must
// stamp the tombstone's clock deterministically), so non-primaries forward it.
void Node::handleDelete(const vector<string> &f, const string &rawPayload, int client_fd) {
    const string key = field(f, 1);
    auto owners = router_->findOwners(key, 1);
    if (owners.empty()) {
        reply(client_fd, "RESPONSE|ERROR|no_nodes_in_ring");
        return;
    }
    const NodeInfo &primary = owners[0];
    if (primary.node_id != info.node_id) {
        TCPClient client;
        string resp = client.sendAndReceiveFramed(primary.host, primary.port, rawPayload,
                                                  static_cast<int>(cfg_.timeout.count()) * 2);
        bool ok = !resp.empty();
        jlog::op(ok ? "info" : "warn", "delete", key, ok ? "forwarded" : "forward_failed");
        reply(client_fd, ok ? resp : "RESPONSE|ERROR|forward_failed");
        return;
    }

    VectorClock context = VectorClock::parse(field(f, 5));
    int N = effN(toInt(f, 3));
    int W = effW(toInt(f, 4));

    PutResult pr = coordinator_->coordinateDelete(key, context, N, W);
    jlog::op(pr.ok ? "info" : "warn", "delete", key, pr.ok ? "ok" : pr.error);
    if (pr.ok) {
        reply(client_fd, "RESPONSE|OK|" + pr.clock.serialize());
    } else {
        reply(client_fd, "RESPONSE|ERROR|" + pr.error);
    }
}

// REPLICATE|<key>|<b64value>|<origin>|<clock>  (coordinator -> replica write)
// The never-regress rule itself lives in replica_ops so the in-process test
// cluster exercises this exact code rather than a copy of it.
void Node::handleReplicate(const vector<string> &f, int client_fd) {
    const string key = field(f, 1);
    VersionedValue incoming{base64::decode(field(f, 2)), VectorClock::parse(field(f, 4))};

    replica_ops::applyReplicate(*storage_, key, incoming);
    // Ack either way: whether we stored the incoming version or kept a strictly
    // newer local one, our copy is at least as new as the coordinator's.
    reply(client_fd, "RESPONSE|OK");
}

// READ|<key>|<origin>  (coordinator -> replica read)
void Node::handleReadReplica(const vector<string> &f, int client_fd) {
    const string key = field(f, 1);
    auto v = replica_ops::readLocal(*storage_, key);
    if (!v) {
        reply(client_fd, "VAL|NOTFOUND");
        return;
    }
    reply(client_fd, "VAL|" + base64::encode(v->data) + "|" + v->clock.serialize());
}

// RING|<origin>  → read-only snapshot of the physical ring. Mutates nothing —
// it's what the gateway calls to render the live ring without disturbing
// membership. All membership changes go through gossip (SWIM_JOIN / LEAVE); the
// old legacy JOIN verb, which added any caller straight to the router with no
// Swim, incarnation, or Left-tombstone check, was removed in Tier 4.7 — it had no
// remaining sender and was a single-frame ring-poisoning hole.
void Node::handleRingQuery(int client_fd) {
    vector<NodeInfo> nodes = router_->getAllPhysicalNodes();
    ostringstream oss;
    oss << "RING\n" << nodes.size() << "\n";
    for (const auto &n : nodes) {
        oss << n.node_id << "|" << n.host << "|" << n.port << "\n";
    }
    reply(client_fd, oss.str());
}

// LEAVE|<node_id>  → permanently remove a node from the cluster.
//
// The operator's half of Dynamo's temporary-vs-permanent distinction. Gossip can
// only ever conclude "unreachable", which is why an unreachable node keeps its
// ring slots; deciding a node is gone FOR GOOD is a judgement no timeout can make,
// so it arrives here instead.
//
// Send this to any live node, not to the target — the target is typically the one
// that is already gone. That node tombstones the departure and gossips it out.
void Node::handleLeave(const vector<string> &f, int client_fd) {
    const string node_id = field(f, 1);
    if (node_id.empty()) {
        reply(client_fd, "RESPONSE|ERROR|missing_node_id");
        return;
    }
    if (!gossip_) {
        // Membership only exists when gossip is running; without it the ring is
        // whatever JOIN built, and there is nothing to disseminate a Leave to.
        reply(client_fd, "RESPONSE|ERROR|gossip_disabled");
        return;
    }
    if (!gossip_->requestLeave(node_id)) {
        reply(client_fd, "RESPONSE|ERROR|unknown_node");
        return;
    }
    reply(client_fd, "RESPONSE|OK|left");
}
