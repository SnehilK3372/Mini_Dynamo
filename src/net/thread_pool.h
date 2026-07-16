#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// A fixed-size worker thread pool. The TCP server dispatches each accepted
// connection to the pool as one task, replacing thread-per-connection: the
// number of live handler threads is bounded by the worker count rather than by
// the number of clients, so gossip + hint + anti-entropy traffic (which Tier 4
// added) can't spawn unbounded threads.
//
// A worker runs one task to completion before taking the next. Because the
// server's task is "handle this connection until it closes", a worker is
// occupied for a connection's lifetime — the pool is connection-bound, which is
// sufficient for a pooled cluster of this size (true C10K would need epoll; see
// docs/decisions/tier-4.3.md). Size the pool above the expected number of
// concurrent inbound connections.
class ThreadPool {
   public:
    explicit ThreadPool(size_t workers);

    // Stops accepting new work, wakes all workers, and joins them. Tasks already
    // dequeued run to completion; tasks still queued are dropped (the process is
    // shutting down, so there is nothing left to serve them).
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // Queue a task for a worker to run. No-op after shutdown() (returns false).
    bool enqueue(std::function<void()> task);

    // Drain: stop accepting, wake workers, join. Idempotent; also called by the
    // destructor. Exposed so a server can shut the pool down deterministically.
    void shutdown();

    size_t workerCount() const { return workers_.size(); }

   private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopping_ = false;
};
