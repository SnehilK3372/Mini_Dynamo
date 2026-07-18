#pragma once
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "node_info.h"
#include "util.h"

using namespace std;

class Router {
   public:
    Router(int vnodes = 128);

    void addPhysicalNode(const NodeInfo &node);
    void removePhysicalNode(const string &node_id);

    vector<NodeInfo> findOwners(const string &key, int replicas = 3);

    vector<pair<uint64_t, string>> debugRing();

    vector<NodeInfo> getAllPhysicalNodes();

    // O(1) count of physical nodes (no membership copy) — feeds the
    // ring_physical_nodes gauge on every membership change.
    size_t physicalCount() const;

   private:
    int virtual_nodes;
    map<uint64_t, string> ring;
    map<string, NodeInfo> nodes;

    mutable shared_mutex mtx_;

    void insertVNode(const NodeInfo &node, int replica_index);
};
