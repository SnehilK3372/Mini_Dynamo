#include "antientropy_thread.h"

#include <algorithm>
#include <random>

#include "../router.h"
#include "../util.h"
#include "../vector_clock.h"

AntiEntropyThread::AntiEntropyThread(const NodeInfo &self, Router *router, StorageEngine *storage,
                                     ExchangeTreeFn exchange_fn, PullKeysFn pull_fn,
                                     PushKeyFn push_fn, std::chrono::seconds interval)
    : self_(self),
      router_(router),
      storage_(storage),
      exchange_fn_(std::move(exchange_fn)),
      pull_fn_(std::move(pull_fn)),
      push_fn_(std::move(push_fn)),
      interval_(interval) {}

AntiEntropyThread::~AntiEntropyThread() { stop(); }

void AntiEntropyThread::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&AntiEntropyThread::run, this);
}

void AntiEntropyThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AntiEntropyThread::run() {
    while (running_) {
        // Sleep first, then tick (gives the cluster time to stabilize on startup).
        for (int i = 0; i < static_cast<int>(interval_.count()) && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_) break;
        tick();
    }
}

MerkleTree AntiEntropyThread::buildLocalTree() {
    MerkleTree tree;
    storage_->forEach([&](const std::string &key, const std::string &value) {
        uint64_t kh = hash64(key);
        uint64_t vh = hash64(value);
        tree.insert(kh, vh);
    });
    tree.build();
    return tree;
}

void AntiEntropyThread::tick() {
    // Pick a random replica peer from the ring (any node except self).
    auto all = router_->getAllPhysicalNodes();
    std::vector<NodeInfo> peers;
    for (const auto &n : all) {
        if (n.node_id != self_.node_id) peers.push_back(n);
    }
    if (peers.empty()) return;

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, peers.size() - 1);
    const NodeInfo &peer = peers[dist(rng)];

    // Build our local Merkle tree.
    MerkleTree local_tree = buildLocalTree();
    if (local_tree.rootHash() == 0) return;  // empty store, nothing to sync

    // Exchange trees with the peer.
    MerkleTree peer_tree = exchange_fn_(peer, local_tree);
    if (peer_tree.rootHash() == 0 && local_tree.rootHash() == 0) return;

    // Find divergent ranges.
    auto diffs = MerkleTree::diff(local_tree, peer_tree);
    if (diffs.empty()) {
        syncs_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // For each divergent range: pull keys from peer, merge locally.
    for (const auto &dr : diffs) {
        auto remote_keys = pull_fn_(peer, dr.range_start, dr.range_end);
        for (const auto &[key, remote_vv] : remote_keys) {
            auto local_stored = storage_->get(key);
            if (!local_stored) {
                // We don't have this key — store it.
                storage_->put(key, remote_vv.serialize());
                keys_repaired_.fetch_add(1, std::memory_order_relaxed);
            } else {
                VersionedValue local_vv = VersionedValue::deserialize(*local_stored);
                auto ord = VectorClock::compare(remote_vv.clock, local_vv.clock);
                if (ord == VectorClock::Ordering::A_DOMINATES) {
                    // Remote is newer — take it.
                    storage_->put(key, remote_vv.serialize());
                    keys_repaired_.fetch_add(1, std::memory_order_relaxed);
                } else if (ord == VectorClock::Ordering::B_DOMINATES) {
                    // We are newer — push to peer.
                    push_fn_(peer, key, local_vv);
                    keys_repaired_.fetch_add(1, std::memory_order_relaxed);
                }
                // CONCURRENT or EQUAL: leave as-is (concurrent versions are
                // resolved by the client on the next read).
            }
        }
    }

    syncs_.fetch_add(1, std::memory_order_relaxed);
}
