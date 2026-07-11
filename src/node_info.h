#pragma once

#include <cstdint>
#include <string>

// Identity + address of one cluster member. Lives in its own header (rather than
// in node.h) so the pure-logic layer — Router, Coordinator, ReplicaClient — can
// depend on it without pulling in the Node class and its networking, and without
// the node.h ↔ coordinator.h include cycle that would otherwise form.
struct NodeInfo {
    std::string node_id;
    std::string host;
    uint16_t port = 0;

    NodeInfo() = default;
    NodeInfo(const std::string &id, const std::string &h, uint16_t p)
        : node_id(id), host(h), port(p) {}
};
