#include "gossip/swim.h"

#include <gtest/gtest.h>

#include "gossip/member_event.h"

using namespace gossip;

class SwimTest : public ::testing::Test {
   protected:
    NodeInfo self{"node1", "host1", 5001};
    Swim swim{self, 5};
};

TEST_F(SwimTest, NewMemberJoin) {
    MemberEvent ev;
    ev.type = EventType::Join;
    ev.node_id = "node2";
    ev.host = "host2";
    ev.port = 5002;
    ev.incarnation = 0;

    EXPECT_TRUE(swim.applyEvent(ev));
    EXPECT_EQ(swim.memberCount(), 1u);
    EXPECT_TRUE(swim.isAlive("node2"));
}

TEST_F(SwimTest, DuplicateJoinIgnored) {
    MemberEvent ev;
    ev.type = EventType::Join;
    ev.node_id = "node2";
    ev.host = "host2";
    ev.port = 5002;
    ev.incarnation = 0;

    EXPECT_TRUE(swim.applyEvent(ev));
    EXPECT_FALSE(swim.applyEvent(ev));
    EXPECT_EQ(swim.memberCount(), 1u);
}

TEST_F(SwimTest, SuspectTransition) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.suspect("node2");
    // Still counted as alive for probing purposes.
    EXPECT_TRUE(swim.isAlive("node2"));
    EXPECT_EQ(swim.memberCount(), 1u);
}

TEST_F(SwimTest, ConfirmDead) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.confirmDead("node2");
    EXPECT_FALSE(swim.isAlive("node2"));
    EXPECT_EQ(swim.memberCount(), 0u);
}

TEST_F(SwimTest, AliveOverridesSuspicion) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.suspect("node2");

    // Higher incarnation alive overrides suspicion.
    MemberEvent alive;
    alive.type = EventType::Alive;
    alive.node_id = "node2";
    alive.host = "host2";
    alive.port = 5002;
    alive.incarnation = 1;

    EXPECT_TRUE(swim.applyEvent(alive));
    auto members = swim.allMembers();
    ASSERT_EQ(members.size(), 1u);
    EXPECT_EQ(members[0].state, MemberState::Alive);
    EXPECT_EQ(members[0].incarnation, 1u);
}

TEST_F(SwimTest, StaleAliveDoesNotOverrideSuspect) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 1;
    swim.applyEvent(join);

    // Suspect at incarnation 1.
    MemberEvent sus;
    sus.type = EventType::Suspect;
    sus.node_id = "node2";
    sus.incarnation = 1;
    swim.applyEvent(sus);

    // Stale alive at incarnation 0 — should be ignored.
    MemberEvent alive;
    alive.type = EventType::Alive;
    alive.node_id = "node2";
    alive.host = "host2";
    alive.port = 5002;
    alive.incarnation = 0;

    EXPECT_FALSE(swim.applyEvent(alive));
}

TEST_F(SwimTest, SelfSuspicionRefutes) {
    // Someone suspects us at our current incarnation.
    MemberEvent sus;
    sus.type = EventType::Suspect;
    sus.node_id = "node1";  // self
    sus.incarnation = 0;

    EXPECT_TRUE(swim.applyEvent(sus));
    // Our incarnation should have been bumped.
    EXPECT_GT(swim.incarnation(), 0u);
}

TEST_F(SwimTest, StaleSelfSuspicionIgnored) {
    // Bump incarnation manually.
    swim.refute();  // now incarnation = 1

    MemberEvent sus;
    sus.type = EventType::Suspect;
    sus.node_id = "node1";  // self
    sus.incarnation = 0;    // stale

    EXPECT_FALSE(swim.applyEvent(sus));
}

TEST_F(SwimTest, DeadCannotBeRevived) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.confirmDead("node2");

    // A stale alive should not revive a dead node — requires a fresh Join.
    MemberEvent alive;
    alive.type = EventType::Alive;
    alive.node_id = "node2";
    alive.host = "host2";
    alive.port = 5002;
    alive.incarnation = 0;

    EXPECT_FALSE(swim.applyEvent(alive));
    EXPECT_FALSE(swim.isAlive("node2"));
}

// REGRESSION (found by the pre-deploy audit, not by a test): a process that
// restarts begins again at incarnation 0, while the cluster still holds it Dead at
// incarnation >= 0. Requiring a strictly-higher incarnation to revive stranded a
// healthy restarted node as Dead *forever* — it received no traffic, and because
// hinted handoff triggers on Dead -> Alive, its hints never delivered and expired
// silently. A direct Join must be honoured whatever incarnation it carries.
//
// FreshJoinRevivesDead (below) missed this by using an artificially higher
// incarnation (5) — it encoded the bug's assumption instead of a real restart.
TEST_F(SwimTest, RestartedNodeRejoinsAtSameIncarnation) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);
    swim.confirmDead("node2");
    ASSERT_FALSE(swim.isAlive("node2"));

    // The node restarts: same id, same address, incarnation back to 0, and speaks
    // for itself through the join handshake.
    MemberEvent rejoin;
    rejoin.type = EventType::Join;
    rejoin.node_id = "node2";
    rejoin.host = "host2";
    rejoin.port = 5002;
    rejoin.incarnation = 0;  // NOT higher — a fresh process

    auto eff = swim.applyDirectJoin(rejoin);
    ASSERT_TRUE(eff.has_value()) << "a plain Dead node's join must be accepted";
    EXPECT_TRUE(swim.isAlive("node2")) << "a restarted node must be able to rejoin";
    // The reviver must bump past its own stale Dead record, or peers still holding
    // Dead@0 would reject the relayed Alive@0 and the node would rejoin for this
    // node only — invisible to the rest of the cluster.
    EXPECT_GT(*eff, 0u) << "the effective incarnation must beat the stale Dead record";
}

// The flip side: relayed gossip is NOT authoritative. A Join event forwarded by a
// third party must obey the incarnation rules, or a stale Join circulating in the
// dissemination stream would resurrect a genuinely dead node.
TEST_F(SwimTest, RelayedJoinCannotReviveDead) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);
    swim.confirmDead("node2");

    EXPECT_FALSE(swim.applyEvent(join)) << "a relayed Join must not resurrect a dead node";
    EXPECT_FALSE(swim.isAlive("node2"));
}

// A suspected node must not be cleared by a same-incarnation relay. Only the node
// itself may refute, by bumping its incarnation. Accepting `>=` here let a stale
// event clear suspicion — and since gossip re-enqueues anything that changed
// state, that event circulated forever and the node could never be declared Dead.
TEST_F(SwimTest, SameIncarnationRelayCannotClearSuspicion) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);
    swim.suspect("node2");

    MemberEvent relayed_alive;
    relayed_alive.type = EventType::Alive;
    relayed_alive.node_id = "node2";
    relayed_alive.host = "host2";
    relayed_alive.port = 5002;
    relayed_alive.incarnation = 0;  // same incarnation the suspicion was raised at

    EXPECT_FALSE(swim.applyEvent(relayed_alive))
        << "same-incarnation relay must not clear suspicion (it never dies out)";

    // A genuine refutation — strictly newer — still wins.
    relayed_alive.incarnation = 1;
    EXPECT_TRUE(swim.applyEvent(relayed_alive));
    EXPECT_TRUE(swim.isAlive("node2"));
}

// The guard still has to do its job: a *relayed* Alive (third-party gossip) must
// not resurrect a dead node without a strictly newer incarnation.
TEST_F(SwimTest, StaleAliveGossipStillCannotReviveDead) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);
    swim.confirmDead("node2");

    MemberEvent stale_alive;
    stale_alive.type = EventType::Alive;  // relayed, not a direct join
    stale_alive.node_id = "node2";
    stale_alive.host = "host2";
    stale_alive.port = 5002;
    stale_alive.incarnation = 0;

    EXPECT_FALSE(swim.applyEvent(stale_alive));
    EXPECT_FALSE(swim.isAlive("node2"));
}

// Relayed gossip carrying a strictly NEWER incarnation does revive a dead node —
// that is how a rejoin propagates to third parties, once the reviver has bumped
// past its stale record (see applyDirectJoin).
//
// Note this test alone is not evidence that restarts work: it hands over an
// artificially higher incarnation (5), whereas a real restarted process comes back
// at 0. That gap is exactly how the rejoin bug survived a green suite — see
// RestartedNodeRejoinsAtSameIncarnation above, and Cluster.RestartedNodeRejoins.
TEST_F(SwimTest, NewerIncarnationGossipRevivesDead) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.confirmDead("node2");

    MemberEvent rejoin;
    rejoin.type = EventType::Alive;
    rejoin.node_id = "node2";
    rejoin.host = "host2";
    rejoin.port = 5002;
    rejoin.incarnation = 5;  // strictly newer than the Dead record

    EXPECT_TRUE(swim.applyEvent(rejoin));
    EXPECT_TRUE(swim.isAlive("node2"));
}

TEST_F(SwimTest, RandomPeersExcludesSpecified) {
    for (int i = 2; i <= 6; ++i) {
        MemberEvent ev;
        ev.type = EventType::Join;
        ev.node_id = "node" + std::to_string(i);
        ev.host = "host" + std::to_string(i);
        ev.port = static_cast<uint16_t>(5000 + i);
        ev.incarnation = 0;
        swim.applyEvent(ev);
    }

    auto peers = swim.randomPeers(3, {"node2", "node3"});
    for (const auto &p : peers) {
        EXPECT_NE(p.node_id, "node2");
        EXPECT_NE(p.node_id, "node3");
    }
}

TEST_F(SwimTest, ExpireSuspectsConfirmsDead) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.suspect("node2");

    // Expire with a zero timeout → should confirm dead immediately.
    auto expired = swim.expireSuspects(std::chrono::milliseconds(0));
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0], "node2");
    EXPECT_FALSE(swim.isAlive("node2"));
}

TEST_F(SwimTest, DisseminationQueueDrains) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);
    swim.enqueueEvent(join);

    // Get events repeatedly until the queue drains.
    int rounds = 0;
    while (true) {
        auto events = swim.getEventsToSend();
        if (events.empty()) break;
        ++rounds;
        if (rounds > 100) break;  // safety
    }
    EXPECT_GT(rounds, 0);
    EXPECT_LE(rounds, 100);
}

// --- MemberEvent serialization tests ---

TEST(MemberEventTest, SerializeDeserializeJoin) {
    MemberEvent ev;
    ev.type = EventType::Join;
    ev.node_id = "node5";
    ev.host = "10.0.0.5";
    ev.port = 5005;
    ev.incarnation = 42;

    std::string s = ev.serialize();
    MemberEvent parsed = MemberEvent::deserialize(s);

    EXPECT_EQ(parsed.type, EventType::Join);
    EXPECT_EQ(parsed.node_id, "node5");
    EXPECT_EQ(parsed.host, "10.0.0.5");
    EXPECT_EQ(parsed.port, 5005);
    EXPECT_EQ(parsed.incarnation, 42u);
}

TEST(MemberEventTest, SerializeDeserializeSuspect) {
    MemberEvent ev;
    ev.type = EventType::Suspect;
    ev.node_id = "nodeX";
    ev.host = "";
    ev.port = 0;
    ev.incarnation = 7;

    std::string s = ev.serialize();
    MemberEvent parsed = MemberEvent::deserialize(s);

    EXPECT_EQ(parsed.type, EventType::Suspect);
    EXPECT_EQ(parsed.node_id, "nodeX");
    EXPECT_EQ(parsed.incarnation, 7u);
}

TEST(MemberEventTest, MultiEventSerializeRoundTrip) {
    std::vector<MemberEvent> events;
    MemberEvent e1;
    e1.type = EventType::Join;
    e1.node_id = "a";
    e1.host = "h1";
    e1.port = 1;
    e1.incarnation = 0;
    events.push_back(e1);

    MemberEvent e2;
    e2.type = EventType::Dead;
    e2.node_id = "b";
    e2.host = "";
    e2.port = 0;
    e2.incarnation = 3;
    events.push_back(e2);

    std::string s = serializeEvents(events);
    auto parsed = deserializeEvents(s);

    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0].node_id, "a");
    EXPECT_EQ(parsed[0].type, EventType::Join);
    EXPECT_EQ(parsed[1].node_id, "b");
    EXPECT_EQ(parsed[1].type, EventType::Dead);
}

TEST(MemberEventTest, EmptyEventsRoundTrip) {
    std::string s = serializeEvents({});
    auto parsed = deserializeEvents(s);
    EXPECT_TRUE(parsed.empty());
}

// --- MemberChange callback test ---

TEST_F(SwimTest, CallbackFiredOnJoinAndDeath) {
    std::vector<std::pair<std::string, MemberState>> changes;
    swim.onMemberChange([&](const NodeInfo &info, MemberState state) {
        changes.push_back({info.node_id, state});
    });

    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.confirmDead("node2");

    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].first, "node2");
    EXPECT_EQ(changes[0].second, MemberState::Alive);
    EXPECT_EQ(changes[1].first, "node2");
    EXPECT_EQ(changes[1].second, MemberState::Dead);
}

// ---- Permanent removal (Tier 4.6) ---------------------------------------
//
// The tombstone rules. Every test below exists because some path could otherwise
// undo an operator's decision, and the feature is worthless if a decommissioned
// node can find ANY way back into the ring.

namespace {

void addMember(Swim &s, const std::string &id, uint64_t incarnation = 0) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = id;
    join.host = "host-" + id;
    join.port = 5002;
    join.incarnation = incarnation;
    s.applyEvent(join);
}

MemberEvent leaveEvent(const std::string &id, uint64_t incarnation = 0) {
    MemberEvent ev;
    ev.type = EventType::Leave;
    ev.node_id = id;
    ev.incarnation = incarnation;
    return ev;
}

}  // namespace

TEST_F(SwimTest, LeaveRemovesFromMembership) {
    addMember(swim, "node2");
    ASSERT_EQ(swim.memberCount(), 1u);

    swim.leave("node2");

    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);
    EXPECT_FALSE(swim.isAlive("node2"));
    EXPECT_EQ(swim.memberCount(), 0u) << "a departed node must not count as a member";
    EXPECT_TRUE(swim.allMembers().empty())
        << "a departed node must not be advertised to joiners — that is what stops a "
           "fresh node from learning about it and re-adding it";
}

// THE CRUX. applyDirectJoin is deliberately not gated on incarnation (a restarted
// process returns at 0 and must still be believed — the Tier-testing rejoin fix),
// so the tombstone is the ONLY thing between a decommissioned node and the ring.
// If this ever fails, decommission silently does nothing the moment its target
// reboots.
TEST_F(SwimTest, LeaveIsStickyAgainstDirectJoin) {
    addMember(swim, "node2");
    swim.leave("node2");

    // The decommissioned node restarts and announces itself through the handshake,
    // exactly as a legitimately restarting node would.
    MemberEvent rejoin;
    rejoin.type = EventType::Join;
    rejoin.node_id = "node2";
    rejoin.host = "host-node2";
    rejoin.port = 5002;
    rejoin.incarnation = 0;

    auto eff = swim.applyDirectJoin(rejoin);

    EXPECT_FALSE(eff.has_value())
        << "a refused join must return nullopt so the caller disseminates NO Alive — an "
           "Alive here would be rejected by peers holding the tombstone but accepted by "
           "any that do not, reviving the node in part of the cluster";
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);
    EXPECT_FALSE(swim.isAlive("node2")) << "a decommissioned node must not rejoin";
}

// The contrast that gives the test above its meaning: the SAME handshake, at the
// SAME incarnation, against a merely-Dead node MUST succeed. Dead and Left differ
// in exactly this. Without the pair, LeaveIsStickyAgainstDirectJoin would also pass
// if applyDirectJoin were broken outright.
TEST_F(SwimTest, DirectJoinStillRevivesAMerelyDeadNode) {
    addMember(swim, "node2");
    swim.confirmDead("node2");

    MemberEvent rejoin;
    rejoin.type = EventType::Join;
    rejoin.node_id = "node2";
    rejoin.host = "host-node2";
    rejoin.port = 5002;
    rejoin.incarnation = 0;

    EXPECT_TRUE(swim.applyDirectJoin(rejoin).has_value())
        << "Dead is a temporary failure — a restart must still bring it back";
    EXPECT_TRUE(swim.isAlive("node2"));
}

// A decommissioned node that is still RUNNING keeps gossiping, and refutes its way
// to ever-higher incarnations. No incarnation may buy it back in.
TEST_F(SwimTest, LeaveIsStickyAgainstRelayedAlive) {
    addMember(swim, "node2");
    swim.leave("node2");

    MemberEvent alive;
    alive.type = EventType::Alive;
    alive.node_id = "node2";
    alive.host = "host-node2";
    alive.port = 5002;
    alive.incarnation = 9999;  // absurdly newer, and still not enough

    EXPECT_FALSE(swim.applyEvent(alive))
        << "the tombstone outranks incarnation entirely; it is not a staleness check";
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);
    EXPECT_FALSE(swim.isAlive("node2"));
}

// An operator's decision is not a stale observation to be outvoted. Gating Leave on
// incarnation would make decommission fail against a node whose incarnation races
// ahead — a flapping node, which is the one you most want to retire.
TEST_F(SwimTest, LeaveWinsRegardlessOfIncarnation) {
    addMember(swim, "node2", /*incarnation=*/9);

    EXPECT_TRUE(swim.applyEvent(leaveEvent("node2", /*incarnation=*/0)))
        << "a Leave at incarnation 0 must still retire a member held at 9";
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);
}

// The common case: reclaiming the ring slots of a node that is already gone.
TEST_F(SwimTest, LeaveWorksOnAnAlreadyDeadNode) {
    addMember(swim, "node2");
    swim.confirmDead("node2");
    ASSERT_EQ(swim.stateOf("node2"), MemberState::Dead);

    swim.leave("node2");
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left)
        << "Dead -> Left is the whole point: a Dead node keeps its ring slots forever "
           "until someone declares it permanently gone";
}

// Left is terminal in both directions: nothing may demote it to a revivable state,
// or the next accepted Alive would put it back in the ring.
TEST_F(SwimTest, LeaveIsTerminalAgainstSuspectAndDead) {
    addMember(swim, "node2");
    swim.leave("node2");

    EXPECT_FALSE(swim.applyEvent(leaveEvent("node2")))
        << "a repeated Leave must not re-fire, or it would gossip forever";

    MemberEvent suspect;
    suspect.type = EventType::Suspect;
    suspect.node_id = "node2";
    suspect.incarnation = 50;
    EXPECT_FALSE(swim.applyEvent(suspect));
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);

    MemberEvent dead;
    dead.type = EventType::Dead;
    dead.node_id = "node2";
    dead.incarnation = 50;
    EXPECT_FALSE(swim.applyEvent(dead));
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);

    swim.confirmDead("node2");
    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left) << "confirmDead must not demote Left";
}

// A Leave can overtake the Alive it refers to, since both race through the same
// dissemination stream. Recording a tombstone for a node we have never heard of is
// what stops the trailing Alive from adding a departed node to our ring alone.
TEST_F(SwimTest, LeaveForAnUnknownNodeStillTombstones) {
    EXPECT_TRUE(swim.applyEvent(leaveEvent("ghost")))
        << "the event must be recorded and relayed, not dropped";
    EXPECT_EQ(swim.stateOf("ghost"), MemberState::Left);

    MemberEvent late_alive;
    late_alive.type = EventType::Alive;
    late_alive.node_id = "ghost";
    late_alive.host = "host-ghost";
    late_alive.port = 5009;
    late_alive.incarnation = 3;
    EXPECT_FALSE(swim.applyEvent(late_alive)) << "the trailing Alive must lose to the tombstone";
    EXPECT_EQ(swim.memberCount(), 0u);
}

// Suspect and Dead about ourselves are inferences a healthy node is entitled to
// refute. A Leave is not an inference — it is a decision, and refuting it would make
// it impossible to retire any node that is still running.
TEST_F(SwimTest, LeaveAboutSelfIsNotRefuted) {
    uint64_t before = swim.incarnation();
    ASSERT_FALSE(swim.hasLeft());

    // Returning true is precisely how an applied event asks to be re-disseminated:
    // applyIncomingEvents re-enqueues whatever changed state. (Swim::leave() is the
    // other path and enqueues directly — see SelfLeaveEnqueuesItsOwnDissemination.)
    EXPECT_TRUE(swim.applyEvent(leaveEvent(self.node_id, before)))
        << "the order must be applied and relayed onward";

    EXPECT_TRUE(swim.hasLeft());
    EXPECT_EQ(swim.incarnation(), before)
        << "refuting would bump the incarnation and broadcast Alive — the exact fight a "
           "node must not pick with its own retirement";

    EXPECT_FALSE(swim.applyEvent(leaveEvent(self.node_id, before)))
        << "a repeated self-Leave must not re-fire, or it would gossip forever";
}

// The self-Leave that ORIGINATES here (an operator ran LEAVE against this very
// node) has no incoming event to be relayed, so it must enqueue its own — else the
// node retires in silence and every peer keeps routing keys to it.
TEST_F(SwimTest, SelfLeaveEnqueuesItsOwnDissemination) {
    swim.leave(self.node_id);

    auto events = swim.getEventsToSend();
    ASSERT_FALSE(events.empty()) << "peers would never learn of the departure";
    EXPECT_EQ(events[0].type, EventType::Leave);
    EXPECT_EQ(events[0].node_id, self.node_id);
}

// Contrast: self-suspicion IS still refuted. Pins that the branch above is
// special-cased on Leave, rather than self-refutation having broken.
TEST_F(SwimTest, SelfSuspicionIsStillRefutedAfterLeaveHandling) {
    uint64_t before = swim.incarnation();
    MemberEvent suspect;
    suspect.type = EventType::Suspect;
    suspect.node_id = self.node_id;
    suspect.incarnation = before;

    EXPECT_TRUE(swim.applyEvent(suspect));
    EXPECT_GT(swim.incarnation(), before) << "a healthy node must still refute suspicion";
    EXPECT_FALSE(swim.hasLeft());
}

TEST_F(SwimTest, SelfLeaveFiresCallbackSoTheNodeDropsItselfFromItsOwnRing) {
    std::vector<std::pair<std::string, MemberState>> changes;
    swim.onMemberChange([&](const NodeInfo &info, MemberState st) {
        changes.push_back({info.node_id, st});
    });

    swim.leave(self.node_id);

    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].first, self.node_id);
    EXPECT_EQ(changes[0].second, MemberState::Left)
        << "without this the retired node keeps serving from a ring it is no longer in";
    EXPECT_TRUE(swim.hasLeft());
}

TEST_F(SwimTest, LeaveFiresCallbackForRingEviction) {
    addMember(swim, "node2");
    std::vector<std::pair<std::string, MemberState>> changes;
    swim.onMemberChange([&](const NodeInfo &info, MemberState st) {
        changes.push_back({info.node_id, st});
    });

    swim.leave("node2");

    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].first, "node2");
    EXPECT_EQ(changes[0].second, MemberState::Left);
}

TEST_F(SwimTest, LeaveExcludesFromPeerSelection) {
    for (int i = 2; i <= 4; ++i) addMember(swim, "node" + std::to_string(i));
    swim.leave("node3");

    for (const auto &p : swim.alivePeers()) {
        EXPECT_NE(p.node_id, "node3") << "a departed node must never be probed";
    }
    for (const auto &p : swim.randomPeers(10)) {
        EXPECT_NE(p.node_id, "node3") << "a departed node must never be an indirect-probe proxy";
    }
}

TEST_F(SwimTest, StateOfDistinguishesDepartedFromUnknown) {
    addMember(swim, "node2");
    swim.leave("node2");

    EXPECT_EQ(swim.stateOf("node2"), MemberState::Left);
    EXPECT_FALSE(swim.stateOf("never-heard-of-it").has_value())
        << "the LEAVE verb rejects unknown ids on this distinction — tombstoning a typo "
           "would pre-emptively bar a node that legitimately joins under that id later";
}
