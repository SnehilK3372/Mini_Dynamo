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
    // Keys come out sorted regardless of insertion order → deterministic wire form.
    VectorClock v;
    v.set("node2", 1);
    v.set("node1", 3);
    EXPECT_EQ(v.serialize(), "node1:3,node2:1");

    EXPECT_EQ(VectorClock::parse("node1:3,node2:1"), v);
    EXPECT_EQ(VectorClock{}.serialize(), "");
    EXPECT_TRUE(VectorClock::parse("").empty());
    // Full round trip.
    EXPECT_EQ(VectorClock::parse(v.serialize()), v);
}
