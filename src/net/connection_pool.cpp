#include "connection_pool.h"

#include <utility>

ConnectionPool::ConnectionPool(ConnectFn connect, CloseFn close, size_t max_per_peer,
                               std::chrono::seconds idle_ttl)
    : connect_(std::move(connect)),
      close_(std::move(close)),
      max_per_peer_(max_per_peer == 0 ? 1 : max_per_peer),
      idle_ttl_(idle_ttl) {
    reaper_ = std::thread([this] { reaperLoop(); });
}

ConnectionPool::~ConnectionPool() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stopping_ = true;
        // Close everything still idle so no fd leaks on shutdown.
        for (auto &[key, conns] : idle_) {
            for (auto &c : conns) close_(c.fd);
            conns.clear();
        }
    }
    reaper_cv_.notify_all();
    if (reaper_.joinable()) reaper_.join();
}

std::string ConnectionPool::keyOf(const std::string &host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

int ConnectionPool::checkout(const std::string &host, uint16_t port,
                             std::chrono::milliseconds timeout) {
    CounterFn reused_cb;
    CounterFn created_cb;
    int reused_fd = -1;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        created_cb = on_created_;  // snapshot under the lock (setCounters may race)
        auto it = idle_.find(keyOf(host, port));
        if (it != idle_.end() && !it->second.empty()) {
            // Reuse the most-recently-returned connection (LIFO): it's the least
            // likely to have been idle-closed by the peer, and keeps a small
            // working set warm while older ones age out to the reaper.
            reused_fd = it->second.back().fd;
            it->second.pop_back();
            reused_cb = on_reused_;
        }
    }
    if (reused_fd >= 0) {
        // Fire the metric outside the lock so a callback can't re-enter the pool.
        if (reused_cb) reused_cb();
        return reused_fd;
    }
    // No idle connection — open a fresh one (outside the lock; connect may block
    // up to `timeout`).
    int fd = connect_ ? connect_(host, port, timeout) : -1;
    if (fd >= 0 && created_cb) created_cb();
    return fd;
}

void ConnectionPool::checkin(const std::string &host, uint16_t port, int fd) {
    if (fd < 0) return;
    bool close_it = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_) {
            close_it = true;
        } else {
            auto &conns = idle_[keyOf(host, port)];
            if (conns.size() >= max_per_peer_) {
                close_it = true;  // pool full for this peer — don't hoard
            } else {
                conns.push_back({fd, std::chrono::steady_clock::now()});
            }
        }
    }
    if (close_it) close_(fd);
}

void ConnectionPool::discard(int fd) {
    if (fd < 0) return;
    close_(fd);
}

void ConnectionPool::reapExpired() {
    std::vector<int> to_close;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto now = std::chrono::steady_clock::now();
        for (auto &[key, conns] : idle_) {
            auto keep_end = conns.begin();
            for (auto &c : conns) {
                if (now - c.returned_at > idle_ttl_) {
                    to_close.push_back(c.fd);
                } else {
                    *keep_end++ = c;
                }
            }
            conns.erase(keep_end, conns.end());
        }
    }
    for (int fd : to_close) close_(fd);
}

void ConnectionPool::setCounters(CounterFn on_created, CounterFn on_reused) {
    std::lock_guard<std::mutex> lk(mtx_);
    on_created_ = std::move(on_created);
    on_reused_ = std::move(on_reused);
}

size_t ConnectionPool::idleCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t n = 0;
    for (const auto &[key, conns] : idle_) n += conns.size();
    return n;
}

void ConnectionPool::reaperLoop() {
    std::unique_lock<std::mutex> lk(mtx_);
    while (!stopping_) {
        // Wake at half the TTL so a connection lingers at most ~1.5x TTL.
        reaper_cv_.wait_for(lk, idle_ttl_ / 2, [this] { return stopping_; });
        if (stopping_) break;
        lk.unlock();
        reapExpired();
        lk.lock();
    }
}
