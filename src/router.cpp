#include "router.h"

#include <iostream>
#include <mutex>
#include <sstream>

using namespace std;

Router::Router(int vnodes) : virtual_nodes(vnodes) {}

void Router::insertVNode(const NodeInfo &node, int replica_index) {
    ostringstream oss;
    oss << node.node_id << "#vn" << replica_index;
    string vnode_key = oss.str();
    uint64_t h = hash64(vnode_key);
    ring[h] = node.node_id;
}

void Router::addPhysicalNode(const NodeInfo &node) {
    unique_lock lk(mtx_);
    if (nodes.find(node.node_id) != nodes.end()) return;
    nodes[node.node_id] = node;
    for (int i = 0; i < virtual_nodes; ++i) {
        insertVNode(node, i);
    }
}

void Router::removePhysicalNode(const string &node_id) {
    unique_lock lk(mtx_);
    if (nodes.find(node_id) == nodes.end()) return;
    nodes.erase(node_id);
    for (auto it = ring.begin(); it != ring.end();) {
        if (it->second == node_id)
            it = ring.erase(it);
        else
            ++it;
    }
}

size_t Router::physicalCount() const {
    shared_lock lk(mtx_);
    return nodes.size();
}

vector<pair<uint64_t, string>> Router::debugRing() {
    shared_lock lk(mtx_);
    vector<pair<uint64_t, string>> out;
    for (auto &p : ring) out.push_back(p);
    return out;
}

vector<NodeInfo> Router::findOwners(const string &key, int replicas) {
    vector<NodeInfo> owners;
    if (replicas <= 0) replicas = 1;
    uint64_t h = hash64(key);

    shared_lock lk(mtx_);
    if (ring.empty()) return owners;

    auto it = ring.lower_bound(h);
    if (it == ring.end()) it = ring.begin();

    set<string> chosen_physical;
    while ((int)owners.size() < replicas && !ring.empty()) {
        string phys = it->second;
        if (nodes.find(phys) != nodes.end() && chosen_physical.insert(phys).second) {
            owners.push_back(nodes[phys]);
        }
        ++it;
        if (it == ring.end()) it = ring.begin();
        if ((int)chosen_physical.size() == (int)nodes.size()) break;
    }
    return owners;
}

vector<NodeInfo> Router::getAllPhysicalNodes() {
    shared_lock lk(mtx_);
    vector<NodeInfo> all_nodes;
    for (const auto &pair : nodes) {
        all_nodes.push_back(pair.second);
    }
    return all_nodes;
}
