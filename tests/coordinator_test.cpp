#include "coordinator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "router.h"
#include "storage/in_memory_storage.h"
#include "metrics.h"

using namespace std::chrono_literals;
using std::string;

// ---------------------------------------------------------------------------
// A fully in-process ReplicaClient. It lets a test program each peer's read
// result, mark peers "down" (never acknowledge) or "slow" (block past the
// coordinator deadline), and capture every write pushed to a peer — including
// the read-repair writes, which the test synchronizes on via waitForWrites()
// instead of sleeping.
// ---------------------------------------------------------------------------
class FakeReplicaClient : public ReplicaClient {
public:
    std::set<string> down;                            // ok=false for read and write
    std::set<string> slowWrite;                       // block ~3x timeout, then proceed
    std::map<string, ReplicaReadResult> readProgram;  // per-node programmed read

    ReplicaWriteResult writeReplica(const NodeInfo &peer, const string &,
                                    const VersionedValue &v,
                                    std::chrono::milliseconds timeout) override {
        InflightGuard g(inflight_);
        if (slowWrite.count(peer.node_id)) std::this_thread::sleep_for(timeout * 3);
        if (down.count(peer.node_id)) return {false};
        {
            std::lock_guard<std::mutex> lk(m_);
            writes_.push_back({peer.node_id, v});
            lastWrite_[peer.node_id] = v;
        }
        cv_.notify_all();
        return {true};
    }

    ReplicaReadResult readReplica(const NodeInfo &peer, const string &,
                                  std::chrono::milliseconds) override {
        InflightGuard g(inflight_);
        if (down.count(peer.node_id)) return {false, false, {}};
        auto it = readProgram.find(peer.node_id);
        if (it != readProgram.end()) return it->second;
        return {true, false, {}};  // responded, key absent
    }

    // Block until at least n writes have been captured (or timeout). This is the
    // explicit synchronization the read-repair test uses — no sleeps, no races.
    bool waitForWrites(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, timeout, [&] { return writes_.size() >= n; });
    }
    size_t writeCount() {
        std::lock_guard<std::mutex> lk(m_);
        return writes_.size();
    }
    bool lastWriteTo(const string &node, VersionedValue &out) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = lastWrite_.find(node);
        if (it == lastWrite_.end()) return false;
        out = it->second;
        return true;
    }

    // Drain in-flight (possibly slow) threads before the fake is destroyed, so a
    // late detached thread never touches a freed object.
    ~FakeReplicaClient() {
        while (inflight_.load() > 0) std::this_thread::yield();
    }

private:
    struct InflightGuard {
        std::atomic<int> &c;
        explicit InflightGuard(std::atomic<int> &c_) : c(c_) { c.fetch_add(1); }
        ~InflightGuard() { c.fetch_sub(1); }
    };
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::pair<string, VersionedValue>> writes_;
    std::map<string, VersionedValue> lastWrite_;
    std::atomic<int> inflight_{0};
};

namespace {
NodeInfo n(const string &id) { return NodeInfo(id, id, 5000); }

ReplicaReadResult foundVersion(const string &data, const VectorClock &clock) {
    ReplicaReadResult r;
    r.ok = true;
    r.found = true;
    r.value = VersionedValue{data, clock};
    return r;
}

VectorClock clk(std::initializer_list<std::pair<const string, uint64_t>> init) {
    VectorClock v;
    for (auto &kv : init) v.set(kv.first, kv.second);
    return v;
}

// Fixture: a 3-node ring (node1=self coordinator, node2, node3), an in-memory
// store for the coordinator's own shard, a fake for the peers, a short quorum
// timeout so the slow-replica test stays fast.
struct Fixture {
    Router router;
    InMemoryStorageEngine storage;
    InMemoryMetrics metrics;
    FakeReplicaClient replicas;
    QuorumConfig cfg;

    Fixture() {
        router.addPhysicalNode(n("node1"));
        router.addPhysicalNode(n("node2"));
        router.addPhysicalNode(n("node3"));
        cfg.timeout = 150ms;
    }
    Coordinator make() {
        return Coordinator(n("node1"), &router, &storage, &replicas, &metrics, cfg);
    }
};
}  // namespace

// ---- Write quorum -------------------------------------------------------

TEST(Coordinator, PutSucceedsWhenWMet) {
    Fixture f;
    auto c = f.make();
    PutResult r = c.coordinatePut("k", "v", VectorClock{}, /*N=*/3, /*W=*/2);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.clock.get("node1"), 1u);  // coordinator bumped its own entry
}

TEST(Coordinator, PutFailsWhenWNotMet) {
    Fixture f;
    f.replicas.down = {"node2", "node3"};  // only the local write can ack
    auto c = f.make();
    PutResult r = c.coordinatePut("k", "v", VectorClock{}, 3, 2);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "quorum_not_met");
}

TEST(Coordinator, PutToleratesOneDownReplica) {
    Fixture f;
    f.replicas.down = {"node3"};  // local + node2 = 2 acks meets W=2
    auto c = f.make();
    PutResult r = c.coordinatePut("k", "v", VectorClock{}, 3, 2);
    EXPECT_TRUE(r.ok);
}

// The requested case: an ack that arrives after the deadline must not count, and
// the coordinator must not hang waiting for it.
TEST(Coordinator, PutSlowReplicaAckDoesNotCountAndDoesNotHang) {
    Fixture f;
    f.replicas.slowWrite = {"node3"};  // node3 blocks ~3x timeout (past the deadline)
    auto c = f.make();

    auto start = std::chrono::steady_clock::now();
    // W=3 needs all three; local + node2 = 2 acks; node3's ack lands too late.
    PutResult r = c.coordinatePut("k", "v", VectorClock{}, /*N=*/3, /*W=*/3);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "quorum_not_met");
    // Returned near the deadline, not after the slow replica's ~450ms block.
    EXPECT_LT(elapsed, f.cfg.timeout * 2)
        << "coordinator waited on the slow replica instead of abandoning it";
}

// ---- Read quorum, reconciliation, siblings ------------------------------

TEST(Coordinator, GetReturnsDominantVersion) {
    Fixture f;
    // Local (node1) holds an older version; node2 holds a strictly newer one.
    f.storage.put("k", (VersionedValue{"old", clk({{"node1", 1}})}).serialize());
    f.replicas.readProgram["node2"] = foundVersion("new", clk({{"node1", 2}}));
    f.replicas.down = {"node3"};
    auto c = f.make();

    GetResult r = c.coordinateGet("k", /*N=*/3, /*R=*/2);
    ASSERT_EQ(r.status, GetResult::Status::OK);
    ASSERT_EQ(r.values.size(), 1u);
    EXPECT_EQ(r.values[0].data, "new");
}

TEST(Coordinator, GetReturnsSiblingsWhenConcurrent) {
    Fixture f;
    f.storage.put("k", (VersionedValue{"a", clk({{"node1", 1}})}).serialize());
    f.replicas.readProgram["node2"] = foundVersion("b", clk({{"node2", 1}}));  // concurrent
    f.replicas.down = {"node3"};
    auto c = f.make();

    GetResult r = c.coordinateGet("k", 3, 2);
    ASSERT_EQ(r.status, GetResult::Status::SIBLINGS);
    ASSERT_EQ(r.values.size(), 2u);
    std::set<string> got{r.values[0].data, r.values[1].data};
    EXPECT_EQ(got, (std::set<string>{"a", "b"}));
    EXPECT_EQ(f.metrics.readRepairCount(), 0u);  // never repair across a conflict
}

TEST(Coordinator, GetNotFoundWhenNoReplicaHasKey) {
    Fixture f;
    f.replicas.down = {"node3"};  // node1 (local) empty, node2 responds "not found"
    auto c = f.make();
    GetResult r = c.coordinateGet("missing", 3, 2);
    EXPECT_EQ(r.status, GetResult::Status::NOTFOUND);
}

TEST(Coordinator, GetFailsWhenRNotMet) {
    Fixture f;
    f.replicas.down = {"node2", "node3"};  // only local responds; R=2 unreachable
    auto c = f.make();
    GetResult r = c.coordinateGet("k", 3, 2);
    EXPECT_EQ(r.status, GetResult::Status::ERROR);
    EXPECT_EQ(r.error, "quorum_not_met");
}

// ---- Read repair --------------------------------------------------------

TEST(Coordinator, ReadRepairPushesDominantToStaleReplica) {
    Fixture f;
    // Local holds the dominant version; node2 answers with a stale one; node3 down.
    f.storage.put("k", (VersionedValue{"new", clk({{"node1", 2}})}).serialize());
    f.replicas.readProgram["node2"] = foundVersion("old", clk({{"node1", 1}}));
    f.replicas.down = {"node3"};
    auto c = f.make();

    GetResult r = c.coordinateGet("k", 3, 2);
    // The read returns immediately with the winner...
    ASSERT_EQ(r.status, GetResult::Status::OK);
    EXPECT_EQ(r.values[0].data, "new");

    // ...and the repair is pushed to the stale replica out of band. Synchronize
    // on it landing rather than sleeping.
    ASSERT_TRUE(f.replicas.waitForWrites(1, 2000ms));
    VersionedValue repaired;
    ASSERT_TRUE(f.replicas.lastWriteTo("node2", repaired));
    EXPECT_EQ(repaired.data, "new");
    EXPECT_EQ(repaired.clock.get("node1"), 2u);
    EXPECT_EQ(f.metrics.readRepairCount(), 1u);
}

// ---- End-to-end clock progression (dominance across sequential writes) ---

TEST(Coordinator, SequentialWritesProduceDominatingClocks) {
    Fixture f;
    auto c = f.make();
    PutResult first = c.coordinatePut("k", "v1", VectorClock{}, 3, 2);
    ASSERT_TRUE(first.ok);
    // Client echoes the clock it just got as context for the next write.
    PutResult second = c.coordinatePut("k", "v2", first.clock, 3, 2);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(VectorClock::compare(second.clock, first.clock), VectorClock::Ordering::A_DOMINATES);
}
