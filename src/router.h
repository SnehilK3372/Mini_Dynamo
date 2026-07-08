#pragma once
#include "util.h"
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <set>
#include "node.h"

using namespace std;

class Router{
public:
    Router(int vnodes = 3);

    void addPhysicalNode(const NodeInfo &node);
    void removePhysicalNode(const string &node_id);

    vector<NodeInfo> findOwners(const string &key, int replicas=3);

    vector<pair<uint64_t, string>> debugRing();
    
    vector<NodeInfo> getAllPhysicalNodes();
private:
    int virtual_nodes; // number of virtual nodes per physical node
    // ring: map from vnode_hash -> physical node_id#replicaIndex (value stores physical id)
    map<uint64_t, string> ring;
    map<string, NodeInfo> nodes;

    mutex mtx;

    void insertVNode(const NodeInfo &node, int replica_index);
};
