#include <gtest/gtest.h>

#include "gossip/member_event.h"
#include "gossip/swim.h"

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

TEST_F(SwimTest, FreshJoinRevivesDead) {
    MemberEvent join;
    join.type = EventType::Join;
    join.node_id = "node2";
    join.host = "host2";
    join.port = 5002;
    join.incarnation = 0;
    swim.applyEvent(join);

    swim.confirmDead("node2");

    // A new Join with higher incarnation revives the node.
    MemberEvent rejoin;
    rejoin.type = EventType::Join;
    rejoin.node_id = "node2";
    rejoin.host = "host2";
    rejoin.port = 5002;
    rejoin.incarnation = 5;

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
