#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// A per-peer pool of reusable open connections, keyed by "host:port". Replaces
// the one-shot connect/close in TCPClient for internal replica traffic: a
// caller checks out a connection (reusing an idle one or creating a fresh one),
// uses it for a framed request/response, then checks it back in for the next
// caller. This removes the connect()/close() syscalls and TIME_WAIT churn that
// Tier 4's per-second gossip + hint + anti-entropy traffic would otherwise
// generate.
//
// The pool holds no POSIX dependency: it is injected with a ConnectFn (open a
// socket to host:port, or return -1 on failure) and a CloseFn (close an fd).
// Production passes POSIX implementations; unit tests pass fakes and exercise
// the checkout/checkin/cap/reap bookkeeping without real sockets. Connections
// are plain int fds — the pool only tracks and hands them out; framing lives in
// the caller (PooledReplicaClient).
class ConnectionPool {
   public:
    // Opens a connection to host:port within `timeout`. Returns an fd (>= 0) or
    // -1 on failure. Must be thread-safe.
    using ConnectFn = std::function<int(const std::string &host, uint16_t port,
                                        std::chrono::milliseconds timeout)>;
    // Closes an fd the pool is done with. Must be thread-safe.
    using CloseFn = std::function<void(int fd)>;
    // Optional metric hooks, fired on a fresh connect vs. an idle reuse.
    using CounterFn = std::function<void()>;

    ConnectionPool(ConnectFn connect, CloseFn close, size_t max_per_peer = 4,
                   std::chrono::seconds idle_ttl = std::chrono::seconds(60));
    ~ConnectionPool();

    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    // Hand out a connection to host:port: reuse the most-recently-returned idle
    // one if any, else create a fresh one via ConnectFn. Returns -1 if a fresh
    // connect fails. The returned fd is owned by the caller until checkin/discard.
    int checkout(const std::string &host, uint16_t port, std::chrono::milliseconds timeout);

    // Return a healthy connection for reuse. If the peer already holds
    // max_per_peer idle connections, the surplus is closed instead of pooled.
    void checkin(const std::string &host, uint16_t port, int fd);

    // Permanently close a connection (send/recv failed, or its response wasn't
    // fully drained so the stream may be desynced). Never returns it to the pool.
    void discard(int fd);

    // Close every idle connection older than the idle TTL. Called periodically by
    // the reaper thread; exposed for deterministic testing.
    void reapExpired();

    // Set metric callbacks (created = fresh connect, reused = idle checkout).
    void setCounters(CounterFn on_created, CounterFn on_reused);

    // Total idle connections currently pooled across all peers (for tests/metrics).
    size_t idleCount() const;

   private:
    struct Idle {
        int fd;
        std::chrono::steady_clock::time_point returned_at;
    };

    void reaperLoop();

    ConnectFn connect_;
    CloseFn close_;
    size_t max_per_peer_;
    std::chrono::seconds idle_ttl_;

    mutable std::mutex mtx_;
    std::unordered_map<std::string, std::vector<Idle>> idle_;  // "host:port" -> idle conns

    CounterFn on_created_;
    CounterFn on_reused_;

    // Background idle reaper.
    std::thread reaper_;
    std::condition_variable reaper_cv_;
    bool stopping_ = false;

    static std::string keyOf(const std::string &host, uint16_t port);
};
