#pragma once

#include <memory>
#include <optional>
#include <string>

#include "message.h"
#include "storage/storage_engine.h"

using namespace std;

class Router;

struct NodeInfo {
    string node_id;
    string host;
    uint16_t port;
    NodeInfo() = default;

    NodeInfo(const string &id, const string &h, uint16_t p): node_id(id), host(h), port(p) {}
};

// A single cluster member. Owns its shard of the data (behind StorageEngine)
// and acts as coordinator for requests that land on it: routing via the
// Router's hash ring, replicating PUTs, and answering the wire protocol in
// onMessageReceived. The TCP accept loop lives in TCPServer, which calls back
// into onMessageReceived — this class has no networking loop of its own.
class Node {
public:
    // Storage is injected so tests can pass a fake and Tier 1A can pass
    // RocksDB without touching coordinator logic.
    Node(const NodeInfo &info, Router *router, unique_ptr<StorageEngine> storage);

    // wire-protocol dispatch (called by TCPServer per accepted connection)
    void onMessageReceived(const Message &msg, int client_fd);

    // storage handlers
    string handlePut(const string &key, const string &value);
    optional<string> handleGet(const string &key);
    void handleReplicate(const string &key, const string &value);

    NodeInfo info;

private:
    Router *router;
    unique_ptr<StorageEngine> storage;
};
