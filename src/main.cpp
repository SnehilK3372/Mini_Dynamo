#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "antientropy/antientropy_thread.h"
#include "base64.h"
#include "gossip/gossip_thread.h"
#include "hints/handoff_thread.h"
#include "hints/hint_store.h"
#include "log.h"
#include "metrics.h"
#include "net/connection_pool.h"
#include "net/pooled_replica_client.h"
#include "net/tcp_client.h"
#include "net/tcp_connect.h"
#include "net/tcp_server.h"
#include "node.h"
#include "router.h"
#include "storage/in_memory_storage.h"
#ifdef HAVE_ROCKSDB
#include "storage/rocksdb_storage.h"
#endif
#ifdef HAVE_PROMETHEUS
#include "metrics_prometheus.h"
#endif

using namespace std;

static string getenv_str(const string &var, const string &default_val = "") {
    const char *val = getenv(var.c_str());
    return val ? string(val) : default_val;
}

static vector<string> split_csv(const string &s) {
    vector<string> out;
    if (s.empty()) return out;
    istringstream iss(s);
    string token;
    while (getline(iss, token, ',')) {
        // Trim whitespace.
        size_t start = token.find_first_not_of(' ');
        if (start == string::npos) continue;
        size_t end = token.find_last_not_of(' ');
        out.push_back(token.substr(start, end - start + 1));
    }
    return out;
}

static unique_ptr<StorageEngine> makeStorage(const string &node_id) {
#ifdef HAVE_ROCKSDB
    string engine = getenv_str("STORAGE_ENGINE", "rocksdb");
    if (engine != "memory") {
        string dir = getenv_str("DATA_DIR", "/data/" + node_id);
        jlog::msg("info", "storage: RocksDB at " + dir);
        return make_unique<RocksDBStorageEngine>(dir);
    }
#else
    string engine = getenv_str("STORAGE_ENGINE", "memory");
    (void)engine;
#endif
    jlog::msg("info", "storage: in-memory (non-durable)");
    return make_unique<InMemoryStorageEngine>();
}

static unique_ptr<Metrics> makeMetrics(const string &node_id) {
#ifdef HAVE_PROMETHEUS
    string bind = "0.0.0.0:" + getenv_str("METRICS_PORT", "9100");
    jlog::msg("info", "metrics: Prometheus exposer on " + bind + "/metrics");
    return make_unique<PrometheusMetrics>(bind, node_id);
#else
    (void)node_id;
    return make_unique<InMemoryMetrics>();
#endif
}

int main() {
    cout.setf(ios::unitbuf);

    string node_id = getenv_str("NODE_ID");
    string host = getenv_str("HOST", "0.0.0.0");
    uint16_t port = stoi(getenv_str("NODE_PORT"));

    jlog::init(node_id);

    // SEED_NODES: comma-separated list of "host:port" for initial cluster contact.
    // If empty, this node is the first member (self-bootstraps without contacting anyone).
    string seed_str = getenv_str("SEED_NODES", "");
    vector<string> seeds = split_csv(seed_str);

    // Backward-compatible: BOOTSTRAP_IP/PORT still works as a single seed.
    string bootstrap_ip = getenv_str("BOOTSTRAP_IP", "");
    uint16_t bootstrap_port = static_cast<uint16_t>(stoi(getenv_str("BOOTSTRAP_PORT", "0")));
    if (!bootstrap_ip.empty() && seeds.empty()) {
        seeds.push_back(bootstrap_ip + ":" + to_string(bootstrap_port));
    }

    // ADVERTISE_HOST: the hostname peers use to reach this node. Defaults to
    // NODE_ID, which equals the Docker hostname (container DNS). Override for
    // local multi-process testing (e.g., "localhost") or multi-host deploys.
    string advertise_host = getenv_str("ADVERTISE_HOST", node_id);

    int vnodes = stoi(getenv_str("VNODES", "128"));
    Router router(vnodes);
    NodeInfo myInfo(node_id, advertise_host, port);

    // Metrics is owned by the Node, but the connection pool needs to report into
    // it too — grab a borrowed pointer before handing ownership over. Valid for
    // the whole process, since the Node lives until exit.
    auto metrics = makeMetrics(node_id);
    Metrics *metrics_ptr = metrics.get();

    // Connection pool (Tier 4.3): reuse persistent per-peer sockets for the
    // coordinator's replica fan-out instead of one connect/close per call. POSIX
    // dial/close are injected so the pool itself stays transport-agnostic. The
    // pool must outlive the Node (declared first → destroyed last).
    size_t pool_max = static_cast<size_t>(stoi(getenv_str("POOL_MAX_CONNS_PER_PEER", "4")));
    int pool_reap_s = stoi(getenv_str("POOL_IDLE_REAP_SECONDS", "60"));
    ConnectionPool conn_pool(
        [](const string &h, uint16_t p, chrono::milliseconds t) {
            int ms = static_cast<int>(t.count());
            return tcpconnect::dial(h, static_cast<int>(p), ms, ms);
        },
        [](int fd) { ::close(fd); }, pool_max, chrono::seconds(pool_reap_s));
    conn_pool.setCounters([metrics_ptr] { metrics_ptr->incPoolConnectionCreated(); },
                          [metrics_ptr] { metrics_ptr->incPoolConnectionReused(); });

    auto pooled_replicas = make_unique<PooledReplicaClient>(&conn_pool, node_id);

    // Vector clocks are bounded (Tier 4.5) so they can't grow one entry per
    // coordinator forever as the cluster scales.
    QuorumConfig qcfg;
    qcfg.max_clock_entries = static_cast<size_t>(stoi(getenv_str("MAX_CLOCK_ENTRIES", "20")));

    Node node(myInfo, &router, makeStorage(node_id), move(metrics), qcfg, move(pooled_replicas));

    router.addPhysicalNode(myInfo);

    size_t workers = static_cast<size_t>(stoi(getenv_str("WORKER_THREADS", "64")));
    TCPServer server(host, port, &node, workers);
    thread serverThread([&server]() { server.start(); });
    serverThread.detach();

    jlog::msg("info", "TCP server listening on " + host + ":" + to_string(port) + " (workers=" +
                          to_string(workers) + ", pool_max=" + to_string(pool_max) + ")");

    // The send function used by the gossip thread — wraps TCPClient for framed
    // request/response over a fresh connection (connection pooling is Tier 4.3).
    auto send_fn = [](const string &h, uint16_t p, const string &payload) -> string {
        TCPClient client;
        return client.sendAndReceiveFramed(h, static_cast<int>(p), payload, 500);
    };

    gossip::GossipConfig gcfg;
    gcfg.protocol_period = chrono::milliseconds(stoi(getenv_str("GOSSIP_PERIOD_MS", "1000")));
    gcfg.ping_timeout = chrono::milliseconds(stoi(getenv_str("GOSSIP_PING_TIMEOUT_MS", "200")));
    gcfg.indirect_probe_count = stoi(getenv_str("GOSSIP_K", "3"));
    gcfg.suspicion_mult = stoi(getenv_str("GOSSIP_SUSPICION_MULT", "5"));

    gossip::GossipThread gossip(myInfo, &router, send_fn, gcfg);
    node.setGossipThread(&gossip);

    if (seeds.empty()) {
        jlog::msg("info", "started as initial seed (no SEED_NODES)");
    } else {
        jlog::msg("info", "joining cluster via seeds: " + seed_str +
                              (bootstrap_ip.empty() ? "" : " (from BOOTSTRAP_IP)"));
        gossip.joinViaSeeds(seeds);
    }

    gossip.start();

    // --- Hinted Handoff ---
    int hint_ttl = stoi(getenv_str("HINT_TTL_SECONDS", "10800"));
    HintStore hint_store{chrono::seconds(hint_ttl)};

    // deliver_fn: replicate a hinted value to the recovered node. Counting the
    // delivery here (rather than inside HandoffThread) keeps the thread free of a
    // Metrics dependency while still moving the counter that was previously never
    // incremented by anything.
    auto deliver_fn = [&send_fn, metrics_ptr](const NodeInfo &target, const string &key,
                                              const VersionedValue &value) -> bool {
        string payload = "REPLICATE|" + key + "|" + base64::encode(value.data) + "|hint|" +
                         value.clock.serialize();
        string resp = send_fn(target.host, target.port, payload);
        bool ok = resp.find("RESPONSE|OK") != string::npos;
        if (ok) metrics_ptr->incHintDelivered();
        return ok;
    };

    HandoffThread handoff(&hint_store, deliver_fn);
    handoff.start();

    // Register gossip callback: on recovery (Dead→Alive), notify handoff. On
    // permanent removal, discard the target's hints — they can never be delivered
    // (delivery fires on Dead→Alive, which a departed node can no longer make), so
    // they would otherwise just sit in memory until the TTL swept them.
    gossip.swim().onMemberChange(
        [&handoff, &hint_store](const NodeInfo &info, gossip::MemberState state) {
            if (state == gossip::MemberState::Alive) {
                handoff.notifyRecovery(info);
            } else if (state == gossip::MemberState::Left) {
                size_t dropped = hint_store.dropTarget(info.node_id);
                if (dropped > 0) {
                    jlog::op("info", "hints", info.node_id,
                             "dropped_" + to_string(dropped) + "_hints_node_departed");
                }
            }
        });

    // Inject hint store + liveness check into the coordinator (via the node's
    // public access). The coordinator uses these for sloppy quorum writes.
    node.setHintStore(&hint_store);
    node.setLivenessCheck(
        [&gossip](const string &node_id) -> bool { return gossip.swim().isAlive(node_id); });

    jlog::msg("info", "hinted handoff started (TTL=" + to_string(hint_ttl) + "s)");

    // --- Anti-Entropy ---
    int ae_interval = stoi(getenv_str("ANTIENTROPY_INTERVAL_SECONDS", "300"));

    auto exchange_fn = [&send_fn](const NodeInfo &peer, const MerkleTree &ours) -> MerkleTree {
        (void)ours;
        // Simplified: in a full implementation, serialize the tree and exchange
        // with the peer over the wire. For now, anti-entropy relies on the pull-
        // based key exchange (the merkle comparison triggers range pulls).
        (void)send_fn;
        (void)peer;
        return MerkleTree();
    };

    auto pull_fn = [](const NodeInfo &, uint64_t,
                      uint64_t) -> vector<pair<string, VersionedValue>> { return {}; };

    auto push_fn = [&send_fn](const NodeInfo &peer, const string &key,
                              const VersionedValue &value) -> bool {
        string payload = "REPLICATE|" + key + "|" + base64::encode(value.data) + "|ae|" +
                         value.clock.serialize();
        string resp = send_fn(peer.host, peer.port, payload);
        return resp.find("RESPONSE|OK") != string::npos;
    };

    AntiEntropyThread antientropy(myInfo, &router, node.storage(), exchange_fn, pull_fn, push_fn,
                                  chrono::seconds(ae_interval));
    antientropy.start();

    jlog::msg("info", "anti-entropy started (interval=" + to_string(ae_interval) + "s)");

    while (true) {
        this_thread::sleep_for(chrono::seconds(60));
    }

    return 0;
}
