#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "log.h"
#include "message.h"
#include "metrics.h"
#include "net/tcp_client.h"
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

string getenv_str(const string &var, const string &default_val = "") {
    const char *val = getenv(var.c_str());
    return val ? string(val) : default_val;
}

static vector<string> split_lines_by_newline(const string &s) {
    vector<string> out;
    istringstream iss(s);
    string line;
    while (getline(iss, line, '\n')) {  // Split by newline
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

static vector<string> split_string(const string &s, char delim) {  // msg splitter
    vector<string> out;
    istringstream iss(s);
    string token;
    while (getline(iss, token, delim)) {
        out.push_back(token);
    }
    return out;
}

// Chooses the per-node storage engine. RocksDB (durable, one instance per node's
// own directory — shared-nothing) is the default when the binary was built with
// it; STORAGE_ENGINE=memory forces the volatile map (used by tests and quick
// demos). Without RocksDB compiled in, memory is the only option.
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

// Chooses the metrics backend. When built with prometheus-cpp, each node stands
// up its own scrape endpoint at http://0.0.0.0:<METRICS_PORT>/metrics (Prometheus
// reaches it over the compose network by container name). Without it — e.g. a
// memory-only test build — metrics are counted in process but not exposed.
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

    // loading env variables
    string node_id = getenv_str("NODE_ID");
    string host = getenv_str("HOST", "0.0.0.0");
    uint16_t port = stoi(getenv_str("NODE_PORT"));

    // Bring up structured logging before anything else logs, so every line on
    // stdout — startup included — is a JSON object carrying this node_id.
    jlog::init(node_id);

    string bootstrap_ip = getenv_str("BOOTSTRAP_IP", "");
    uint16_t bootstrap_port = (uint16_t)stoi(getenv_str("BOOTSTRAP_PORT", "0"));

    bool isBootstrap = bootstrap_ip.empty();

    Router router;
    NodeInfo myInfo(node_id, node_id, port);
    Node node(myInfo, &router, makeStorage(node_id), makeMetrics(node_id));

    router.addPhysicalNode(myInfo);  // prevents race condtion

    TCPServer server(host, port, &node);
    thread serverThread([&server]() { server.start(); });
    serverThread.detach();

    jlog::msg("info", "TCP server listening on " + host + ":" + to_string(port));

    if (isBootstrap) {  // logic for bootstrapping node
        jlog::msg("info", "started as BOOTSTRAP node");

    } else {
        // Join existing cluster
        jlog::msg("info",
                  "contacting bootstrap at " + bootstrap_ip + ":" + to_string(bootstrap_port));

        TCPClient client;

        Message joinMsg;
        joinMsg.type = "JOIN";
        joinMsg.origin = node_id;
        joinMsg.key = node_id;
        joinMsg.host = node_id;
        joinMsg.port = myInfo.port;
        joinMsg.value = to_string(myInfo.port);

        string resp =
            client.sendAndReceiveFramed(bootstrap_ip, bootstrap_port, joinMsg.serialize());

        if (!resp.empty()) {
            auto lines = split_lines_by_newline(resp);

            if (!lines.empty() && lines[0] == "RING_UPDATE") {
                if (lines.size() >= 2) {
                    try {
                        int n = stoi(lines[1]);
                        jlog::msg("info",
                                  "JOIN accepted; learning " + to_string(n) + " existing node(s)");

                        for (int i = 0; i < n && 2 + i < (int)lines.size(); ++i) {
                            // Server sends: NODE_ID|HOST|PORT
                            auto parts = split_string(lines[2 + i], '|');

                            if (parts.size() == 3) {
                                string nid = parts[0];
                                string nhost = parts[1];
                                uint16_t nport = (uint16_t)stoi(parts[2]);

                                NodeInfo ni{nid, nhost, nport};
                                router.addPhysicalNode(ni);
                                jlog::op("info", "join", nid, "learned");
                            }
                        }
                    } catch (const exception &e) {
                        jlog::msg("error", string("failed to parse RING_UPDATE: ") + e.what());
                    }
                }
            } else {
                jlog::msg("error", "malformed JOIN response: " + (lines.empty() ? resp : lines[0]));
            }

        } else {
            jlog::msg("error", "JOIN request failed (no response)");
        }
    }
    while (true) {
        this_thread::sleep_for(chrono::seconds(20));
    }

    return 0;
}
