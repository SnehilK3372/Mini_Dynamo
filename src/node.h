#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "message.h"
using json = nlohmann::json;

using namespace std;


class Router;

struct NodeInfo {
    string node_id;
    string host;
    uint16_t port;
    NodeInfo() = default;

    NodeInfo(const string &id, const string &h, uint16_t p): node_id(id), host(h), port(p) {}
};

class Node {
public:
    Node(const NodeInfo &info, Router *router);
    ~Node();

    void start();
    void stop();

    // networking helpers
    void sendMessage(const NodeInfo &dest, const Message &m);
    void onMessageReceived(const Message &msg, int client_fd);


    // storage handlers
    string handlePut(const string &key, const string &value);
    optional<string> handleGet(const string &key);
    void handleReplicate(const string &key, const string &value);

    // debug helpers
    NodeInfo info;


private:
    // server loop (runs in background thread)
    void server_loop(uint16_t port);
        map<string, string> local_storage;
    mutex storage_mutex;

    Router *router;
    atomic<bool> stop_flag;

    // simple in-memory key->value store and its lock
    mutex store_mtx;
    unordered_map<string, string> store;
};

// --- end node.h ---


