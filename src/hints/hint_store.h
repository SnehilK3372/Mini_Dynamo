#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../versioned_value.h"

// A hint: a write that was accepted on behalf of a temporarily-unavailable node.
// Once the target recovers (detected by gossip), the handoff thread delivers it.
struct Hint {
    std::string target_node_id;  // the node this write was meant for
    std::string key;
    VersionedValue value;
    std::chrono::steady_clock::time_point created_at;
};

// Stores hints for offline nodes. In production this would use a RocksDB column
// family for crash durability; for now it's an in-memory store (sufficient to
// demonstrate the protocol and pass the convergence tests, since the hint
// lifetime is short — seconds to minutes, not hours).
class HintStore {
   public:
    explicit HintStore(std::chrono::seconds ttl = std::chrono::seconds(3 * 3600),
                       size_t max_hints_per_target = 10000);

    // Store a hint for a downed target.
    void store(const std::string &target_node_id, const std::string &key,
               const VersionedValue &value);

    // Retrieve and remove all hints for a given target (called on recovery).
    std::vector<Hint> drain(const std::string &target_node_id);

    // Remove expired hints (called periodically).
    void expireOld();

    // Counts for metrics/testing.
    size_t hintCount() const;
    size_t hintCountFor(const std::string &target_node_id) const;

   private:
    std::chrono::seconds ttl_;
    size_t max_per_target_;
    mutable std::mutex mtx_;
    // target_node_id -> list of hints
    std::unordered_map<std::string, std::vector<Hint>> hints_;
};
