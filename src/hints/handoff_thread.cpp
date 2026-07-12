#include "handoff_thread.h"

HandoffThread::HandoffThread(HintStore *store, DeliverFn deliver_fn,
                             std::chrono::milliseconds poll_interval)
    : store_(store), deliver_fn_(std::move(deliver_fn)), poll_interval_(poll_interval) {}

HandoffThread::~HandoffThread() { stop(); }

void HandoffThread::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&HandoffThread::run, this);
}

void HandoffThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void HandoffThread::notifyRecovery(const NodeInfo &recovered) {
    std::lock_guard<std::mutex> lk(queue_mtx_);
    recovery_queue_.push_back(recovered);
}

void HandoffThread::run() {
    while (running_) {
        std::vector<NodeInfo> targets;
        {
            std::lock_guard<std::mutex> lk(queue_mtx_);
            targets.swap(recovery_queue_);
        }

        for (const auto &target : targets) {
            deliverFor(target);
        }

        // Also expire old hints periodically.
        store_->expireOld();

        std::this_thread::sleep_for(poll_interval_);
    }
}

void HandoffThread::deliverFor(const NodeInfo &target) {
    auto hints = store_->drain(target.node_id);
    for (const auto &hint : hints) {
        if (deliver_fn_(target, hint.key, hint.value)) {
            delivered_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Delivery failed — re-store the hint for a future attempt.
            store_->store(target.node_id, hint.key, hint.value);
        }
    }
}
