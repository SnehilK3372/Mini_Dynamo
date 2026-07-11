#include "node.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include "base64.h"
#include "log.h"
#include "message.h"
#include "net/framing.h"
#include "net/tcp_client.h"
#include "net/tcp_replica_client.h"
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
           unique_ptr<Metrics> metrics, QuorumConfig cfg)
    : info(info_),
      router_(router),
      storage_(move(storage)),
      metrics_(metrics ? move(metrics) : make_unique<InMemoryMetrics>()),
      replicas_(make_unique<TcpReplicaClient>(info_.node_id)),
      cfg_(cfg) {
    coordinator_ = make_unique<Coordinator>(info, router_, storage_.get(), replicas_.get(),
                                            metrics_.get(), cfg_);
}

void Node::handleRequest(const string &payload, int client_fd) {
    auto f = splitAll(payload, '|');
    if (f.empty() || f[0].empty()) return;
    const string &type = f[0];

    // Client-facing ops are timed and counted here at the protocol edge (the
    // internal REPLICATE/READ/JOIN/RING verbs are cluster plumbing, not client
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
    } else if (type == "JOIN") {
        handleJoin(payload, client_fd);
    } else if (type == "RING") {
        handleRingQuery(client_fd);
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
void Node::handleReplicate(const vector<string> &f, int client_fd) {
    const string key = field(f, 1);
    VersionedValue incoming{base64::decode(field(f, 2)), VectorClock::parse(field(f, 4))};

    // Never regress: if we already hold a version that strictly dominates the
    // incoming one, this is a stale/duplicate replicate — acknowledge it (our
    // copy is at least as new) but keep ours. Otherwise store the incoming
    // version. Concurrent versions across *different* replicas are surfaced as
    // siblings at read time; a single replica keeps one value (full per-replica
    // sibling storage is deferred with anti-entropy).
    auto stored = storage_->get(key);
    if (stored) {
        VersionedValue local = VersionedValue::deserialize(*stored);
        if (VectorClock::compare(local.clock, incoming.clock) ==
            VectorClock::Ordering::A_DOMINATES) {
            reply(client_fd, "RESPONSE|OK");
            return;
        }
    }
    storage_->put(key, incoming.serialize());
    reply(client_fd, "RESPONSE|OK");
}

// READ|<key>|<origin>  (coordinator -> replica read)
void Node::handleReadReplica(const vector<string> &f, int client_fd) {
    const string key = field(f, 1);
    auto stored = storage_->get(key);
    if (!stored) {
        reply(client_fd, "VAL|NOTFOUND");
        return;
    }
    VersionedValue v = VersionedValue::deserialize(*stored);
    reply(client_fd, "VAL|" + base64::encode(v.data) + "|" + v.clock.serialize());
}

// JOIN|<id>|<port>|<origin>|<host>|<port>  → reply with the current ring.
void Node::handleJoin(const string &payload, int client_fd) {
    Message msg = Message::deserialize(payload);
    if (msg.key.empty() || msg.host.empty() || msg.port == 0) {
        jlog::op("warn", "join", msg.key, "malformed");
        return;
    }
    NodeInfo joiner{msg.key, msg.host, static_cast<uint16_t>(msg.port)};
    jlog::op("info", "join", joiner.node_id, "added");

    // Snapshot the ring *before* adding the joiner, then add it.
    vector<NodeInfo> current = router_->getAllPhysicalNodes();
    router_->addPhysicalNode(joiner);

    ostringstream oss;
    oss << "RING_UPDATE\n" << current.size() << "\n";
    for (const auto &n : current) {
        oss << n.node_id << "|" << n.host << "|" << n.port << "\n";
    }
    reply(client_fd, oss.str());
}

// RING|<origin>  → read-only snapshot of the physical ring. Unlike JOIN (which
// returns the ring *and* adds the caller), this mutates nothing — it's what the
// gateway calls to render the live ring without disturbing membership. A distinct
// header ("RING", not "RING_UPDATE") keeps the two responses unambiguous.
void Node::handleRingQuery(int client_fd) {
    vector<NodeInfo> nodes = router_->getAllPhysicalNodes();
    ostringstream oss;
    oss << "RING\n" << nodes.size() << "\n";
    for (const auto &n : nodes) {
        oss << n.node_id << "|" << n.host << "|" << n.port << "\n";
    }
    reply(client_fd, oss.str());
}
