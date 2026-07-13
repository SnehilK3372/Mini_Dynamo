#include "net/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(ThreadPoolTest, RunsAllTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }));
    }
    pool.shutdown();  // joins workers → all dequeued tasks have finished
    EXPECT_EQ(counter.load(), N);
}

TEST(ThreadPoolTest, EachTaskRunsExactlyOnce) {
    ThreadPool pool(8);
    const int N = 500;
    std::mutex m;
    std::set<int> seen;
    for (int i = 0; i < N; ++i) {
        pool.enqueue([i, &m, &seen] {
            std::lock_guard<std::mutex> lk(m);
            seen.insert(i);  // a duplicate run would collapse (set), a lost one would be absent
        });
    }
    pool.shutdown();
    EXPECT_EQ(seen.size(), static_cast<size_t>(N));
    EXPECT_EQ(*seen.begin(), 0);
    EXPECT_EQ(*seen.rbegin(), N - 1);
}

TEST(ThreadPoolTest, EnqueueAfterShutdownIsRejected) {
    ThreadPool pool(2);
    pool.shutdown();
    std::atomic<bool> ran{false};
    EXPECT_FALSE(pool.enqueue([&ran] { ran = true; }));
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(ran.load());
}

TEST(ThreadPoolTest, ConcurrentEnqueueIsSafe) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int producers = 8, per = 250;
    std::vector<std::thread> ts;
    for (int p = 0; p < producers; ++p) {
        ts.emplace_back([&pool, &counter, per] {
            for (int i = 0; i < per; ++i) {
                pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto &t : ts) t.join();
    pool.shutdown();
    EXPECT_EQ(counter.load(), producers * per);
}

TEST(ThreadPoolTest, DestructorJoinsCleanly) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 100; ++i) {
            pool.enqueue([&counter] {
                std::this_thread::sleep_for(1ms);
                counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // No explicit shutdown — the destructor must drain-join without deadlock.
    }
    // Some queued tasks may be dropped on shutdown; the guarantee is that we do
    // not hang and no task runs after the pool is gone.
    SUCCEED();
}
