#include <gtest/gtest.h>

#include <string>

#include "support/in_process_cluster.h"

// Multi-node behaviour, exercised against REAL Coordinators, GossipThreads,
// HintStores and HandoffThreads wired in one process (see in_process_cluster.h).
//
// This is the layer the suite was missing. Unit tests stub the cluster away with
// a FakeReplicaClient; e2e.sh needs Docker and covers one scenario. Everything
// here runs in milliseconds and covers the seams where the real bugs lived.
using namespace testcluster;
using Ordering = VectorClock::Ordering;
using namespace std::chrono_literals;

namespace {

// Find a key whose primary owner (per the ring) is `id` — needed to aim a write
// at a specific node's ownership.
std::string keyOwnedBy(NodeCtx &n, const std::string &id, int n_replicas = 3) {
    for (int i = 0; i < 5000; ++i) {
        std::string k = "k" + std::to_string(i);
        auto owners = n.router.findOwners(k, n_replicas);
        if (!owners.empty() && owners[0].node_id == id) return k;
    }
    ADD_FAILURE() << "no key found primaried on " << id;
    return "";
}

// Is `id` among the N owners of `key`?
bool ownsKey(NodeCtx &n, const std::string &key, const std::string &id, int n_replicas = 3) {
    for (const auto &o : n.router.findOwners(key, n_replicas)) {
        if (o.node_id == id) return true;
    }
    return false;
}

}  // namespace

// ---- Membership ---------------------------------------------------------

TEST(Cluster, GossipConvergesToFullRing) {
    InProcessCluster c(5);
    c.startGossip();
    // Every node must learn all 5 — not just the seed, which was the pre-Tier-4.1
    // failure mode (only the bootstrap ever had a complete ring).
    EXPECT_TRUE(c.waitForRingEverywhere(5)) << "gossip did not converge on a 5-node ring";
}

TEST(Cluster, DeadNodeIsDetectedButStaysInTheRing) {
    InProcessCluster c(4);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(4));

    c.kill("node3");
    // Detection: suspicion timeout is protocol_period * suspicion_mult (~60ms).
    EXPECT_TRUE(c.waitForAliveEverywhere("node3", /*alive=*/false))
        << "gossip never detected the dead node";

    // ...but the ring is untouched. A temporary failure must not reshuffle key
    // ownership; the coordinator handles the dead owner per request (skip on
    // reads, hint + stand-in on writes). Evicting here is what made hinted
    // handoff unreachable.
    EXPECT_TRUE(c.waitForRingContains("node3", /*present=*/true))
        << "a transient failure evicted the node from the ring";
}

// The Dynamo distinction, pinned: a node dying must not move anyone's keys.
// Ownership churn on every blip is expensive and, worse, it silently disabled
// hinted handoff for the whole of Tier 4.2.
TEST(Cluster, TemporaryFailureDoesNotReshuffleOwnership) {
    InProcessCluster c(4);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(4));

    NodeCtx &n1 = c.node("node1");
    std::vector<std::vector<std::string>> before;
    for (int i = 0; i < 50; ++i) {
        std::vector<std::string> owners;
        for (const auto &o : n1.router.findOwners("key" + std::to_string(i), 3)) {
            owners.push_back(o.node_id);
        }
        before.push_back(owners);
    }

    c.kill("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node3", false));

    for (int i = 0; i < 50; ++i) {
        std::vector<std::string> owners;
        for (const auto &o : n1.router.findOwners("key" + std::to_string(i), 3)) {
            owners.push_back(o.node_id);
        }
        EXPECT_EQ(owners, before[i]) << "ownership of key" << i << " moved when a node died";
    }
}

// THE REGRESSION. A restarted process comes back at incarnation 0 while peers
// still hold it Dead at >= 0. Requiring a strictly-higher incarnation to revive
// stranded it as Dead forever: alive, healthy, invisible. Found by a live
// kill/restart during the pre-deploy audit — never by a test, because the unit
// test used an artificially higher incarnation.
TEST(Cluster, RestartedNodeRejoins) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    c.kill("node2");
    ASSERT_TRUE(c.waitForAliveEverywhere("node2", false)) << "node2's death was never detected";

    c.restart("node2");  // fresh Swim → incarnation 0, exactly like a new process
    EXPECT_TRUE(c.waitForAliveEverywhere("node2", true))
        << "a restarted node never came back Alive — it would receive no traffic, forever, "
           "and (since hints deliver on Dead->Alive) its hints would silently expire";
    EXPECT_TRUE(c.waitForRingEverywhere(3));
}

// ---- Hinted handoff (Tier 4.2) ------------------------------------------

// HandoffThread's first test. The feature was silently inert for weeks: hints
// deliver on the Dead -> Alive transition, and a restarted node could never make
// that transition, so hints just expired. Nothing noticed, because nothing tested
// it end to end.
TEST(Cluster, HintIsDeliveredOnRecovery) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    NodeCtx &n1 = c.node("node1");
    // A key node2 replicates, coordinated by node1.
    std::string key;
    for (int i = 0; i < 5000 && key.empty(); ++i) {
        std::string k = "hk" + std::to_string(i);
        if (ownsKey(n1, k, "node2") && ownsKey(n1, k, "node1")) key = k;
    }
    ASSERT_FALSE(key.empty()) << "no key co-owned by node1 and node2";

    c.kill("node2");
    ASSERT_TRUE(c.waitForAliveEverywhere("node2", false));

    // Write while node2 is down: sloppy quorum should stash a hint for it.
    PutResult w = n1.coord->coordinatePut(key, "written-during-outage", VectorClock{}, 3, 2);
    ASSERT_TRUE(w.ok) << "write should stay available with one owner down: " << w.error;
    ASSERT_TRUE(c.waitFor([&] { return n1.hints.hintCountFor("node2") > 0; }, 2s))
        << "no hint was stored for the downed owner";

    c.restart("node2");

    // On recovery the handoff thread must push the missed write to node2.
    NodeCtx &n2 = c.node("node2");
    EXPECT_TRUE(c.waitFor(
        [&] {
            auto v = replica_ops::readLocal(n2.storage, key);
            return v && v->data == "written-during-outage";
        },
        3s))
        << "the hint was never delivered to the recovered node";
    EXPECT_TRUE(c.waitFor([&] { return n1.hints.hintCountFor("node2") == 0; }, 2s))
        << "hints were not drained after delivery";
}

// ---- Quorum, repair, siblings across real nodes -------------------------

TEST(Cluster, WriteThenReadRoundTripsAcrossNodes) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    NodeCtx &n1 = c.node("node1");
    ASSERT_TRUE(n1.coord->coordinatePut("hello", "world", VectorClock{}, 3, 2).ok);

    // Any node can coordinate the read and must see it.
    GetResult r = c.node("node3").coord->coordinateGet("hello", 3, 2);
    ASSERT_EQ(r.status, GetResult::Status::OK);
    EXPECT_EQ(r.values[0].data, "world");
}

TEST(Cluster, ReadRepairConvergesStaleReplica) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    NodeCtx &n1 = c.node("node1");
    std::string key = keyOwnedBy(n1, "node1");

    // node3 misses a write (it is down), then comes back holding a stale copy.
    ASSERT_TRUE(n1.coord->coordinatePut(key, "v1", VectorClock{}, 3, 2).ok);
    c.kill("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node3", false));
    PutResult w2 = n1.coord->coordinatePut(key, "v2", VectorClock{}, 3, 2);
    ASSERT_TRUE(w2.ok);
    c.restart("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node3", true));

    // R=3, not R=2, on purpose: a read returns as soon as R responses are in and
    // only repairs replicas that actually answered. With R=2 the local read plus
    // node2 satisfy the quorum and node3's stale copy is never even consulted, so
    // nothing repairs it. R=3 forces every replica into the comparison — the same
    // reason bench/chaos.sh converges with quiet R=3 reads.
    GetResult r = n1.coord->coordinateGet(key, 3, 3);
    ASSERT_EQ(r.status, GetResult::Status::OK);
    EXPECT_EQ(r.values[0].data, "v2");

    NodeCtx &n3 = c.node("node3");
    if (ownsKey(n1, key, "node3")) {
        EXPECT_TRUE(c.waitFor(
            [&] {
                auto v = replica_ops::readLocal(n3.storage, key);
                return v && v->data == "v2";
            },
            3s))
            << "read repair never converged the stale replica";
    }
}

TEST(Cluster, ConcurrentWritesSurfaceAsSiblings) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    // Two different coordinators each write blind (empty context), so neither
    // write saw the other: genuinely concurrent, and both must survive.
    NodeCtx &n1 = c.node("node1");
    NodeCtx &n2 = c.node("node2");
    const std::string key = "conflict";

    // Write directly to each node's own storage with concurrent clocks, bypassing
    // replication, so the replicas genuinely disagree.
    VectorClock c1;
    c1.set("node1", 1, 1000);
    VectorClock c2;
    c2.set("node2", 1, 1000);
    n1.storage.put(key, (VersionedValue{"from-node1", c1}).serialize());
    n2.storage.put(key, (VersionedValue{"from-node2", c2}).serialize());

    // R=3 so every replica answers: with R=2 the read is satisfied by node1 plus
    // whichever peer replies first, and node2's conflicting version may never be
    // consulted — the conflict would be invisible rather than absent.
    GetResult r = n1.coord->coordinateGet(key, 3, 3);
    ASSERT_EQ(r.status, GetResult::Status::SIBLINGS)
        << "concurrent versions must be preserved, not silently collapsed";
    ASSERT_EQ(r.values.size(), 2u);
    EXPECT_EQ(n1.metrics.readRepairCount(), 0u) << "must never repair across a conflict";
}

TEST(Cluster, QuorumFailsWhenTooManyNodesAreDown) {
    QuorumConfig cfg;
    cfg.timeout = 150ms;
    InProcessCluster c(3, cfg);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    c.kill("node2");
    c.kill("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node2", false));

    // Only node1 is left: W=3 is unreachable. It must fail fast and cleanly.
    NodeCtx &n1 = c.node("node1");
    auto start = std::chrono::steady_clock::now();
    PutResult r = n1.coord->coordinatePut("k", "v", VectorClock{}, /*N=*/3, /*W=*/3);
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "quorum_not_met");
    EXPECT_LT(elapsed, 2s) << "a write with dead replicas must not hang";
}
