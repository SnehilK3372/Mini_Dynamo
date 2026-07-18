#include <gtest/gtest.h>

#include <string>

#include "gossip/gossip_thread.h"
#include "router.h"
#include "util.h"

// Node ids ride three nested unescaped text formats ('|' verbs, ';' event lists,
// ':' event fields). CLAUDE.md always documented the constraint; the Tier-4.7
// audit found NOTHING enforced it — one operator typo in NODE_ID poisoned event
// parsing on every node in the cluster. These tests pin the validator and, more
// importantly, the ingress points that must call it.

TEST(NodeId, AcceptsEverythingTheDeploymentsGenerate) {
    for (const char *id :
         {"node1", "kvstore-3", "kv_store.7", "A", "n", "some-host-style.name_01"}) {
        EXPECT_TRUE(isValidNodeId(id)) << id;
    }
    EXPECT_TRUE(isValidNodeId(std::string(64, 'a'))) << "64 chars is the inclusive cap";
}

TEST(NodeId, RejectsDelimitersEmptyAndOversize) {
    for (const char *id :
         {"", "a|b", "a:b", "a;b", "a,b", "a b", "a\nb", "a\rb", "a\tb", "caf\xc3\xa9"}) {
        EXPECT_FALSE(isValidNodeId(id)) << "accepted: '" << id << "'";
    }
    EXPECT_FALSE(isValidNodeId(std::string(65, 'a'))) << "65 chars must be rejected";
}

namespace {

// A GossipThread with a black-hole transport: enough to drive handleMessage, the
// real wire ingress, without any network.
struct Ingress {
    Router router{8};
    gossip::GossipThread gossip;

    Ingress()
        : gossip(
              NodeInfo{"self", "self-host", 5001}, &router,
              [](const std::string &, uint16_t, const std::string &) { return std::string(); },
              gossip::GossipConfig{}) {}
};

}  // namespace

// The apply-time choke point. Defense in depth: an id that smuggles ':' or ';'
// mangles the surrounding parse into truncated-but-plausible ids, which is why
// such ids are refused at their SOURCE (the join handshake / NODE_ID startup
// check below) and can never enter a legitimate node's event stream at all. What
// the apply-time check catches is the residue a corrupted or malformed stream
// still produces — ids that parse out invalid (spaces, control chars) and the
// empty-id events malformed tokens decay to — so they are dropped instead of
// fabricating members and being re-disseminated.
TEST(NodeId, PoisonedPiggybackEventIsDroppedNotApplied) {
    Ingress in;
    // One well-formed event, one whose parsed id is invalid (embedded space), one
    // malformed token (parses to an empty id).
    std::string ping =
        "SWIM_PING|peer|self|"
        "J:good:host-good:5002:0;J:ev il:host-evil:5003:0;garbage";
    in.gossip.handleMessage(ping);

    EXPECT_TRUE(in.gossip.swim().stateOf("good").has_value()) << "the valid event must apply";
    EXPECT_EQ(in.gossip.swim().memberCount(), 1u)
        << "an invalid-id event fabricated a phantom member";
    EXPECT_FALSE(in.gossip.swim().stateOf("ev il").has_value());
}

// The handshake ingress: a joiner with an invalid id gets no ack and no record —
// acking would let a misconfigured node come up believing it joined, and its id
// would corrupt every piggyback list this node emits from then on.
TEST(NodeId, JoinWithInvalidIdIsRefused) {
    Ingress in;
    std::string join = "SWIM_JOIN|bad:id|host-x|5009|0|";
    EXPECT_EQ(in.gossip.handleMessage(join), "") << "no ack for an invalid id";
    EXPECT_EQ(in.gossip.swim().memberCount(), 0u);

    std::string ok = "SWIM_JOIN|fine-id|host-x|5009|0|";
    EXPECT_NE(in.gossip.handleMessage(ok), "") << "a valid join must still be acked";
    EXPECT_TRUE(in.gossip.swim().isAlive("fine-id"));
}
