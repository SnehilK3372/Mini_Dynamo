#include <gtest/gtest.h>

#include <string>
#include <thread>

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

// ---- Permanent removal (Tier 4.6) ---------------------------------------

// The other half of Dynamo's distinction. Gossip can only ever conclude
// "unreachable", which is why DeadNodeIsDetectedButStaysInTheRing holds; deciding
// a node is gone FOR GOOD is an operator's call, and it is the one thing that may
// change the ring.
TEST(Cluster, PermanentRemovalEvictsFromEveryRing) {
    InProcessCluster c(4);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(4));

    // The node is already gone — the case this exists for. Its slots would
    // otherwise be held forever by a corpse.
    c.kill("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node3", false));
    ASSERT_TRUE(c.waitForRingContains("node3", true)) << "precondition: death alone keeps the ring";

    ASSERT_TRUE(c.decommission("node3"));

    // One node was told; gossip must carry it to everyone.
    EXPECT_TRUE(c.waitForLeftEverywhere("node3"))
        << "the departure never propagated beyond the node that was told";
    EXPECT_TRUE(c.waitForRingContains("node3", /*present=*/false))
        << "a permanently removed node must lose its ring slots";
    EXPECT_TRUE(c.waitForRingEverywhere(3));
}

// THE REGRESSION THIS TIER EXISTS TO PREVENT, and the deliberate mirror of
// Cluster.RestartedNodeRejoins. Same actions, opposite requirement — the pair is
// the point. A restarted node MUST come back; a decommissioned one MUST NOT, even
// though both return at incarnation 0 through the same authoritative handshake that
// is (by design) not gated on incarnation.
TEST(Cluster, DecommissionedNodeCannotRejoin) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    ASSERT_TRUE(c.decommission("node2"));
    ASSERT_TRUE(c.waitForLeftEverywhere("node2"));
    ASSERT_TRUE(c.waitForRingContains("node2", false));

    // The retired node's process comes back — a fresh Swim at incarnation 0, and it
    // announces itself through the seed exactly as a legitimate restart would. The
    // cluster must not take it back.
    c.restart("node2");

    // Give gossip real time to get it wrong: several protocol periods, so a leak
    // would actually surface rather than being outrun by the assertion.
    EXPECT_FALSE(
        c.waitFor([&] { return InProcessCluster::ringHas(c.node("node1"), "node2"); }, 500ms))
        << "a decommissioned node walked back into the ring on restart — the operator's "
           "decision silently evaporated the moment the box rebooted";
    EXPECT_FALSE(
        c.waitFor([&] { return InProcessCluster::ringHas(c.node("node3"), "node2"); }, 200ms))
        << "the departed node got back into a peer's ring";
    EXPECT_EQ(c.node("node1").gossipRef()->swim().stateOf("node2"), gossip::MemberState::Left);
}

// The refused joiner must be TOLD, not stonewalled: the ack carries a Leave naming
// it, which trips its own self-Leave branch. Otherwise it spins forever believing
// it is a cluster member, serving reads from a ring nobody else agrees with.
TEST(Cluster, ARefusedJoinerRetiresItself) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    ASSERT_TRUE(c.decommission("node2"));
    ASSERT_TRUE(c.waitForLeftEverywhere("node2"));

    c.restart("node2");  // joins via the seed, which refuses it

    NodeCtx &n2 = c.node("node2");
    EXPECT_TRUE(c.waitFor(
        [&] {
            auto g = n2.gossipRef();
            return g && g->swim().hasLeft();
        },
        2s))
        << "the refused joiner never learned it had been retired";

    EXPECT_FALSE(InProcessCluster::ringHas(n2, "node2"))
        << "a retired node must drop itself from its own ring, or it keeps serving keys "
           "it no longer owns";
}

// The exact mirror of TemporaryFailureDoesNotReshuffleOwnership: same setup, same
// 50 keys, opposite assertion. Together the two pin the temporary-vs-permanent
// distinction as an executable property rather than a paragraph in a doc.
TEST(Cluster, PermanentRemovalReshufflesOwnership) {
    InProcessCluster c(4);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(4));

    NodeCtx &n1 = c.node("node1");
    auto ownersOf = [&](int i) {
        std::vector<std::string> owners;
        for (const auto &o : n1.router.findOwners("key" + std::to_string(i), 3)) {
            owners.push_back(o.node_id);
        }
        return owners;
    };

    std::vector<std::vector<std::string>> before;
    for (int i = 0; i < 50; ++i) before.push_back(ownersOf(i));

    ASSERT_TRUE(c.decommission("node3"));
    ASSERT_TRUE(c.waitForRingContains("node3", false));

    int moved = 0;
    for (int i = 0; i < 50; ++i) {
        auto now = ownersOf(i);
        for (const auto &o : now) {
            ASSERT_NE(o, "node3") << "a departed node must not own key" << i << " any more";
        }
        if (now != before[i]) ++moved;
    }
    EXPECT_GT(moved, 0) << "removal must hand the departed node's ranges to someone — if "
                           "nothing moved, the ring never changed and its keys are lost";
}

// Hints for a departed node can never be delivered: handoff fires on Dead->Alive,
// and Left is terminal. Without an explicit drop they would sit in memory until the
// 3h TTL swept them.
TEST(Cluster, LeaveDropsPendingHints) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    NodeCtx &n1 = c.node("node1");
    std::string key;
    for (int i = 0; i < 5000 && key.empty(); ++i) {
        std::string k = "dk" + std::to_string(i);
        if (ownsKey(n1, k, "node2") && ownsKey(n1, k, "node1")) key = k;
    }
    ASSERT_FALSE(key.empty());

    c.kill("node2");
    ASSERT_TRUE(c.waitForAliveEverywhere("node2", false));
    ASSERT_TRUE(n1.coord->coordinatePut(key, "v", VectorClock{}, 3, 2).ok);
    ASSERT_TRUE(c.waitFor([&] { return n1.hints.hintCountFor("node2") > 0; }, 2s))
        << "precondition: a hint must exist to be dropped";

    // main.cpp wires this to the Left callback; the harness asserts the store's own
    // contract, which is the part that could silently rot.
    ASSERT_TRUE(c.decommission("node2"));
    EXPECT_GT(n1.hints.dropTarget("node2"), 0u);
    EXPECT_EQ(n1.hints.hintCountFor("node2"), 0u)
        << "hints for a node that can never recover must not linger for the full TTL";
}

// ---- Membership anti-entropy (Tier 4.7) ----------------------------------

// THE REGRESSION for the audit's structural finding: gossip events have a finite
// dissemination budget, and before Tier 4.7 the only full membership exchange was
// the one-shot join ack — so an event that a node missed was missed FOREVER. A
// node partitioned through a decommission held the departed node in its ring
// permanently: the operator's LEAVE silently never took there.
TEST(Cluster, PartitionedNodeMissesLeave_SyncConverges) {
    InProcessCluster c(4);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(4));

    // node4 is partitioned — running, stateful, unreachable. NOT killed: a killed
    // node would rejoin via the seed and receive a clean view, hiding the bug.
    c.partition("node4");

    // The decommission target is already DEAD — the canonical use, and the test
    // depends on it: a node decommissioned while still running re-enqueues its
    // own Leave and, since a retired node sends no pings, sits on an undrained
    // copy that the healed node's resurrection probe would collect via plain ack
    // piggyback — converging without ever exercising the sync path (the first
    // draft of this test failed its own sync assertion exactly that way). A dead
    // target holds nothing, so once the live side's copies drain, the tombstone
    // exists only as *state* — and state travels only by sync.
    c.kill("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node3", false));
    ASSERT_TRUE(c.decommission("node3"));
    ASSERT_TRUE(c.waitForLeftEverywhere("node3"));
    ASSERT_TRUE(c.waitForRingContains("node3", false));

    // Let the Leave's piggyback budget drain on the connected side (3·log2(N)
    // sends at a 20ms period) so that when node4 returns there are NO leftover
    // events that could patch it up — only the digest sync can. The drained queue
    // is not observable without consuming it, so this is a bounded sleep, not a
    // poll; the sync-counter assertion below pins the mechanism either way.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    NodeCtx &n4 = c.node("node4");
    ASSERT_TRUE(InProcessCluster::ringHas(n4, "node3"))
        << "precondition broken: the partitioned node should still hold the departed node";

    c.heal("node4");

    // Digest mismatch → push-pull sync → the tombstone lands, the ring shrinks.
    EXPECT_TRUE(c.waitFor(
        [&] {
            auto g = n4.gossipRef();
            return g && g->swim().stateOf("node3") == gossip::MemberState::Left &&
                   !InProcessCluster::ringHas(n4, "node3");
        },
        5s))
        << "the healed node never learned of the decommission — the LEAVE silently "
           "failed on one node, which is the exact divergence the audit flagged";
    EXPECT_TRUE(c.waitForRingEverywhere(3));

    // The fix must have come through a sync, and the tombstone must have survived
    // it in both directions — node4's stale Alive for node3 must not resurrect it.
    EXPECT_GT(n4.membership_syncs.load(), 0u)
        << "node4 converged without a sync — leftover piggyback fixed it, so this "
           "test is not exercising the anti-entropy path";
    for (const auto &id : {"node1", "node2"}) {
        EXPECT_EQ(c.node(id).gossipRef()->swim().stateOf("node3"), gossip::MemberState::Left)
            << id << " lost the tombstone after syncing with the diverged node";
    }
}

// The bonus bug the audit understated: a node partitioned past the suspicion
// timeout is marked Dead by everyone. Pre-4.7, healing the partition could NEVER
// bring it back — peers don't probe Dead nodes, its own pings don't revive it
// (only a JOIN handshake or a strictly-newer Alive can, and both the refutation
// events and joinViaSeeds are long gone). A healthy, running node was stranded
// invisible until a full process restart. Same class as the Tier-testing rejoin
// bug, for partitions instead of restarts.
TEST(Cluster, HealedPartitionRecoversWithoutRestart) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    c.partition("node3");
    ASSERT_TRUE(c.waitForAliveEverywhere("node3", false))
        << "peers never declared the partitioned node dead";

    // Outlast the Dead event's dissemination budget, as a real partition would.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    c.heal("node3");  // no restart, no re-join — the network just comes back

    // node3 pings a peer, the ack's digest disagrees (it holds itself Alive, the
    // peer holds it Dead), the sync delivers its own death notice, and the
    // standard SWIM self-refutation broadcasts Alive at a strictly newer
    // incarnation. No new revival mechanism — just the existing rule, fed by sync.
    EXPECT_TRUE(c.waitForAliveEverywhere("node3", true))
        << "a healed partition stranded a healthy node as Dead forever — it would "
           "receive no traffic and its hints would expire undelivered";
    EXPECT_TRUE(c.waitForRingEverywhere(3));
}

// The gauge that makes all of the above observable: on a converged cluster every
// node reports the same ring size (min==max across the fleet is the Prometheus
// divergence check), and a decommission moves it everywhere.
TEST(Cluster, RingGaugeTracksMembershipEverywhere) {
    InProcessCluster c(3);
    c.startGossip();
    ASSERT_TRUE(c.waitForRingEverywhere(3));

    EXPECT_TRUE(c.waitFor([&] {
        for (const auto &id : {"node1", "node2", "node3"}) {
            if (c.node(id).metrics.ringNodesCount() != 3u) return false;
        }
        return true;
    })) << "gauge does not reflect the converged 3-node ring";

    ASSERT_TRUE(c.decommission("node3"));
    ASSERT_TRUE(c.waitForRingContains("node3", false));
    EXPECT_TRUE(c.waitFor([&] {
        return c.node("node1").metrics.ringNodesCount() == 2u &&
               c.node("node2").metrics.ringNodesCount() == 2u;
    })) << "gauge did not track the decommission";
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
