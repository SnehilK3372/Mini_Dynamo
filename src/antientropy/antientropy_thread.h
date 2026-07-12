#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../node_info.h"
#include "../storage/storage_engine.h"
#include "../versioned_value.h"
#include "merkle_tree.h"

class Router;

// Periodic anti-entropy: builds a Merkle tree of local data, exchanges root
// hashes with a random replica peer, drills down into divergent ranges, and
// pulls/pushes keys to converge. Catches silent divergence that read repair
// misses (keys that are never read).
class AntiEntropyThread {
   public:
    // exchange_fn: sends our Merkle tree data to a peer and receives theirs.
    // Returns the peer's tree (empty on failure).
    using ExchangeTreeFn = std::function<MerkleTree(const NodeInfo &peer, const MerkleTree &ours)>;

    // pull_keys_fn: given a peer and a hash range, pulls all keys in that range
    // from the peer. Returns (key, VersionedValue) pairs.
    using PullKeysFn =
        std::function<std::vector<std::pair<std::string, VersionedValue>>(
            const NodeInfo &peer, uint64_t range_start, uint64_t range_end)>;

    // push_key_fn: push a local key to a peer (same as replica write).
    using PushKeyFn = std::function<bool(const NodeInfo &peer, const std::string &key,
                                         const VersionedValue &value)>;

    AntiEntropyThread(const NodeInfo &self, Router *router, StorageEngine *storage,
                      ExchangeTreeFn exchange_fn, PullKeysFn pull_fn, PushKeyFn push_fn,
                      std::chrono::seconds interval = std::chrono::seconds(300));
    ~AntiEntropyThread();

    void start();
    void stop();

    uint64_t syncsCompleted() const { return syncs_.load(std::memory_order_relaxed); }
    uint64_t keysRepaired() const { return keys_repaired_.load(std::memory_order_relaxed); }

   private:
    void run();
    void tick();
    MerkleTree buildLocalTree();

    NodeInfo self_;
    Router *router_;
    StorageEngine *storage_;
    ExchangeTreeFn exchange_fn_;
    PullKeysFn pull_fn_;
    PushKeyFn push_fn_;
    std::chrono::seconds interval_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<uint64_t> syncs_{0};
    std::atomic<uint64_t> keys_repaired_{0};
};
