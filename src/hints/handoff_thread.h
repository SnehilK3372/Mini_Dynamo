#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../node_info.h"
#include "hint_store.h"

// Delivers stored hints when a recovered node comes back online. Registered as
// a gossip membership callback: when SWIM detects a node transition from Dead →
// Alive, the handoff thread wakes and pushes all hints for that node.
class HandoffThread {
   public:
    // deliver_fn: sends a REPLICATE-equivalent to the target.
    using DeliverFn = std::function<bool(const NodeInfo &target, const std::string &key,
                                         const VersionedValue &value)>;

    HandoffThread(HintStore *store, DeliverFn deliver_fn,
                  std::chrono::milliseconds poll_interval = std::chrono::milliseconds(2000));
    ~HandoffThread();

    void start();
    void stop();

    // Called by the gossip layer when a node recovers (Dead → Alive).
    void notifyRecovery(const NodeInfo &recovered);

    // Metrics.
    uint64_t hintsDelivered() const { return delivered_.load(std::memory_order_relaxed); }

   private:
    void run();
    void deliverFor(const NodeInfo &target);

    HintStore *store_;
    DeliverFn deliver_fn_;
    std::chrono::milliseconds poll_interval_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    mutable std::mutex queue_mtx_;
    std::vector<NodeInfo> recovery_queue_;

    std::atomic<uint64_t> delivered_{0};
};
