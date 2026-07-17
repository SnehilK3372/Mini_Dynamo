#include "vector_clock.h"

#include <gtest/gtest.h>

using Ordering = VectorClock::Ordering;

namespace {
VectorClock clk(std::initializer_list<std::pair<const std::string, uint64_t>> init) {
    VectorClock v;
    for (auto &kv : init) v.set(kv.first, kv.second);
    return v;
}
}  // namespace

TEST(VectorClock, EqualClocksCompareEqual) {
    EXPECT_EQ(VectorClock::compare(clk({{"a", 1}, {"b", 2}}), clk({{"a", 1}, {"b", 2}})),
              Ordering::EQUAL);
    // Absent entry and explicit zero are the same history.
    EXPECT_EQ(VectorClock::compare(clk({{"a", 1}}), clk({{"a", 1}, {"b", 0}})), Ordering::EQUAL);
    EXPECT_EQ(VectorClock::compare(VectorClock{}, VectorClock{}), Ordering::EQUAL);
}

TEST(VectorClock, DominanceIsDetectedBothWays) {
    // b's history is a strict subset of a's → a dominates.
    EXPECT_EQ(VectorClock::compare(clk({{"a", 2}, {"b", 1}}), clk({{"a", 1}, {"b", 1}})),
              Ordering::A_DOMINATES);
    EXPECT_EQ(VectorClock::compare(clk({{"a", 1}, {"b", 1}}), clk({{"a", 2}, {"b", 1}})),
              Ordering::B_DOMINATES);
    // Growing a brand-new node id still counts as strictly ahead.
    EXPECT_EQ(VectorClock::compare(clk({{"a", 1}, {"b", 1}}), clk({{"a", 1}})),
              Ordering::A_DOMINATES);
}

TEST(VectorClock, ConcurrentWhenNeitherDominates) {
    // a ahead on node a, b ahead on node b → a genuine conflict.
    EXPECT_EQ(VectorClock::compare(clk({{"a", 1}}), clk({{"b", 1}})), Ordering::CONCURRENT);
    EXPECT_EQ(VectorClock::compare(clk({{"a", 2}, {"b", 1}}), clk({{"a", 1}, {"b", 2}})),
              Ordering::CONCURRENT);
}

TEST(VectorClock, MergeTakesElementwiseMax) {
    VectorClock m = VectorClock::merge(clk({{"a", 2}, {"b", 1}}), clk({{"a", 1}, {"c", 5}}));
    EXPECT_EQ(m.get("a"), 2u);
    EXPECT_EQ(m.get("b"), 1u);
    EXPECT_EQ(m.get("c"), 5u);
    // The merge dominates both inputs.
    EXPECT_EQ(VectorClock::compare(m, clk({{"a", 2}, {"b", 1}})), Ordering::A_DOMINATES);
}

TEST(VectorClock, IncrementAndGet) {
    VectorClock v;
    EXPECT_EQ(v.get("a"), 0u);
    v.increment("a");
    v.increment("a");
    EXPECT_EQ(v.get("a"), 2u);
}

TEST(VectorClock, SerializeIsCanonicalAndRoundTrips) {
    // Explicit timestamps so the wire form is deterministic to assert on. Keys come
    // out sorted regardless of insertion order → canonical.
    VectorClock v;
    v.set("node2", 1, 1700000000001ULL);
    v.set("node1", 3, 1700000000000ULL);
    EXPECT_EQ(v.serialize(), "node1:3:1700000000000,node2:1:1700000000001");

    EXPECT_EQ(VectorClock{}.serialize(), "");
    EXPECT_TRUE(VectorClock::parse("").empty());
    // Full round trip preserves counters AND timestamps.
    VectorClock rt = VectorClock::parse(v.serialize());
    EXPECT_EQ(rt, v);
    EXPECT_EQ(rt.serialize(), v.serialize());
}

// Tier 4.5 changed the wire form to node:counter:ts. The 2-field legacy form must
// still parse (timestamp 0 → oldest, so it prunes first) rather than being dropped.
TEST(VectorClock, ParsesLegacyTwoFieldFormat) {
    VectorClock legacy = VectorClock::parse("node1:3,node2:1");
    EXPECT_EQ(legacy.get("node1"), 3u);
    EXPECT_EQ(legacy.get("node2"), 1u);
    EXPECT_EQ(legacy.entries().at("node1").updated_ms, 0u);
    // Causally identical to a fresh clock with the same counters, whatever the ts.
    VectorClock modern;
    modern.set("node1", 3, 1700000000000ULL);
    modern.set("node2", 1, 1700000000000ULL);
    EXPECT_EQ(VectorClock::compare(legacy, modern), Ordering::EQUAL);
    EXPECT_EQ(legacy, modern);
}

// Timestamps are pruning metadata, never causality.
TEST(VectorClock, TimestampsDoNotAffectCausality) {
    VectorClock a, b;
    a.set("node1", 5, 1000);
    b.set("node1", 5, 999999);  // same history, wildly different stamp
    EXPECT_EQ(VectorClock::compare(a, b), Ordering::EQUAL);
    EXPECT_EQ(a, b);

    // A newer timestamp must not make a stale counter look ahead.
    VectorClock old_hi, new_lo;
    old_hi.set("node1", 9, 1);       // ahead on the counter, ancient stamp
    new_lo.set("node1", 2, 999999);  // behind on the counter, fresh stamp
    EXPECT_EQ(VectorClock::compare(old_hi, new_lo), Ordering::A_DOMINATES);
}

// ---- Pruning (Tier 4.5) -------------------------------------------------

TEST(VectorClock, PruneIsNoOpWithinBound) {
    VectorClock v;
    for (int i = 0; i < 5; ++i) v.set("node" + std::to_string(i), 1, 1000 + i);
    v.prune(20);
    EXPECT_EQ(v.size(), 5u);
}

TEST(VectorClock, PruneKeepsNewestEntries) {
    VectorClock v;
    // 25 entries, node0 oldest … node24 newest.
    for (int i = 0; i < 25; ++i) v.set("node" + std::to_string(i), 1, 1000 + i);
    ASSERT_EQ(v.size(), 25u);

    v.prune(20);
    EXPECT_EQ(v.size(), 20u);
    // The 5 oldest are gone; the 20 newest survive.
    for (int i = 0; i < 5; ++i) EXPECT_EQ(v.get("node" + std::to_string(i)), 0u) << "i=" << i;
    for (int i = 5; i < 25; ++i) EXPECT_EQ(v.get("node" + std::to_string(i)), 1u) << "i=" << i;
}

TEST(VectorClock, PruneIsDeterministicAcrossIdenticalClocks) {
    // Two nodes handed the same clock must drop the same entries, or replicas
    // would diverge purely from pruning. Equal timestamps force the node-id tiebreak.
    auto build = [] {
        VectorClock v;
        for (int i = 0; i < 10; ++i) v.set("node" + std::to_string(i), 1, 5000);  // all same ts
        return v;
    };
    VectorClock a = build(), b = build();
    a.prune(4);
    b.prune(4);
    EXPECT_EQ(a.size(), 4u);
    EXPECT_EQ(a.serialize(), b.serialize());
}

TEST(VectorClock, PruningDegradesToConcurrentNotFalseDominance) {
    // The realistic shape: a big clock loses its oldest entries but keeps plenty
    // the other side lacks, so the comparison stays conservative (siblings) rather
    // than silently declaring a winner.
    VectorClock big;
    for (int i = 0; i < 25; ++i) big.set("node" + std::to_string(i), 5, 1000 + i);
    VectorClock small;
    small.set("node0", 1, 1000);  // only the entry that big is about to prune

    big.prune(20);  // drops node0..node4
    // big no longer has node0, and small has it → neither strictly dominates.
    EXPECT_EQ(VectorClock::compare(big, small), Ordering::CONCURRENT);
}
