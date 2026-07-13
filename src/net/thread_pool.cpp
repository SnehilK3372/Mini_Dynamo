#include "thread_pool.h"

#include <algorithm>

ThreadPool::ThreadPool(size_t workers) {
    if (workers == 0) workers = 1;  // a zero-worker pool would never run anything
    workers_.reserve(workers);
    for (size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return false;
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto &w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopping_ || !tasks_.empty(); });
            // Drain remaining tasks even while stopping? No — on shutdown we drop
            // queued work (the server is going away). But a worker that already
            // holds a task below runs it to completion.
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();  // run outside the lock so tasks can enqueue/park freely
    }
}
