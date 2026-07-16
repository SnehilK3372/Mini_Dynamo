#include "net/connection_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <vector>

using namespace std::chrono_literals;

// A fake transport: each "connect" hands out a monotonically increasing fake fd
// and records it as open; "close" records it as closed. Lets us assert the
// pool's create/reuse/cap/reap bookkeeping without touching real sockets.
namespace {
struct FakeTransport {
    std::mutex m;
    int next_fd = 100;
    std::set<int> open;  // currently-open fake fds
    std::vector<int> closed;
    int connect_calls = 0;

    ConnectionPool::ConnectFn connectFn() {
        return [this](const std::string &, uint16_t, std::chrono::milliseconds) {
            std::lock_guard<std::mutex> lk(m);
            ++connect_calls;
            int fd = next_fd++;
            open.insert(fd);
            return fd;
        };
    }
    ConnectionPool::CloseFn closeFn() {
        return [this](int fd) {
            std::lock_guard<std::mutex> lk(m);
            open.erase(fd);
            closed.push_back(fd);
        };
    }
    size_t openCount() {
        std::lock_guard<std::mutex> lk(m);
        return open.size();
    }
};
}  // namespace

TEST(ConnectionPoolTest, CheckoutCreatesWhenEmpty) {
    FakeTransport t;
    std::atomic<int> created{0}, reused{0};
    ConnectionPool pool(t.connectFn(), t.closeFn(), 4, 60s);
    pool.setCounters([&] { created++; }, [&] { reused++; });

    int fd = pool.checkout("host", 5001, 100ms);
    EXPECT_GE(fd, 0);
    EXPECT_EQ(created.load(), 1);
    EXPECT_EQ(reused.load(), 0);
    EXPECT_EQ(t.connect_calls, 1);
}

TEST(ConnectionPoolTest, CheckinThenCheckoutReuses) {
    FakeTransport t;
    std::atomic<int> created{0}, reused{0};
    ConnectionPool pool(t.connectFn(), t.closeFn(), 4, 60s);
    pool.setCounters([&] { created++; }, [&] { reused++; });

    int fd1 = pool.checkout("host", 5001, 100ms);
    pool.checkin("host", 5001, fd1);
    EXPECT_EQ(pool.idleCount(), 1u);

    int fd2 = pool.checkout("host", 5001, 100ms);
    EXPECT_EQ(fd2, fd1);           // same connection handed back
    EXPECT_EQ(created.load(), 1);  // no new connect
    EXPECT_EQ(reused.load(), 1);
    EXPECT_EQ(t.connect_calls, 1);
}

TEST(ConnectionPoolTest, CapClosesSurplusOnCheckin) {
    FakeTransport t;
    ConnectionPool pool(t.connectFn(), t.closeFn(), 2, 60s);  // cap = 2 per peer

    // Check out 3 distinct connections, then return all 3.
    int a = pool.checkout("host", 5001, 100ms);
    int b = pool.checkout("host", 5001, 100ms);
    int c = pool.checkout("host", 5001, 100ms);
    ASSERT_EQ(t.openCount(), 3u);

    pool.checkin("host", 5001, a);
    pool.checkin("host", 5001, b);
    pool.checkin("host", 5001, c);  // exceeds cap → this one is closed

    EXPECT_EQ(pool.idleCount(), 2u);
    EXPECT_EQ(t.openCount(), 2u);  // the surplus connection was closed
}

TEST(ConnectionPoolTest, DiscardClosesAndDoesNotPool) {
    FakeTransport t;
    ConnectionPool pool(t.connectFn(), t.closeFn(), 4, 60s);

    int fd = pool.checkout("host", 5001, 100ms);
    pool.discard(fd);
    EXPECT_EQ(pool.idleCount(), 0u);
    EXPECT_EQ(t.openCount(), 0u);  // discarded connection is closed, not pooled
}

TEST(ConnectionPoolTest, ReapClosesIdleConnections) {
    FakeTransport t;
    // Zero TTL → any idle connection is immediately expired on the next reap.
    ConnectionPool pool(t.connectFn(), t.closeFn(), 4, 0s);

    int fd = pool.checkout("host", 5001, 100ms);
    pool.checkin("host", 5001, fd);
    ASSERT_EQ(pool.idleCount(), 1u);

    pool.reapExpired();
    EXPECT_EQ(pool.idleCount(), 0u);
    EXPECT_EQ(t.openCount(), 0u);
}

TEST(ConnectionPoolTest, PeersAreIsolated) {
    FakeTransport t;
    ConnectionPool pool(t.connectFn(), t.closeFn(), 4, 60s);

    int a = pool.checkout("hostA", 5001, 100ms);
    pool.checkin("hostA", 5001, a);

    // A checkout for a different peer must NOT reuse hostA's connection.
    int b = pool.checkout("hostB", 5002, 100ms);
    EXPECT_NE(b, a);
    EXPECT_EQ(t.connect_calls, 2);
    EXPECT_EQ(pool.idleCount(), 1u);  // hostA's still idle, hostB's checked out
}

TEST(ConnectionPoolTest, DestructorClosesIdle) {
    FakeTransport t;
    {
        ConnectionPool pool(t.connectFn(), t.closeFn(), 4, 60s);
        int fd = pool.checkout("host", 5001, 100ms);
        pool.checkin("host", 5001, fd);
        ASSERT_EQ(t.openCount(), 1u);
    }
    EXPECT_EQ(t.openCount(), 0u);  // pool destruction closed the idle connection
}
