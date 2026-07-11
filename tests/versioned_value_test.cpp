#include "versioned_value.h"

#include <gtest/gtest.h>

#include <algorithm>

TEST(VersionedValue, RoundTripsDataAndClock) {
    VersionedValue vv;
    vv.data = "hello";
    vv.clock.set("node1", 2);
    vv.clock.set("node2", 1);

    VersionedValue back = VersionedValue::deserialize(vv.serialize());
    EXPECT_EQ(back.data, "hello");
    EXPECT_EQ(back.clock, vv.clock);
}

TEST(VersionedValue, SurvivesPipesNewlinesAndEmptyClock) {
    VersionedValue vv;
    vv.data = "a|b|c\nrow2";  // data containing the wire delimiter
    // no clock entries

    std::string s = vv.serialize();
    // Exactly one '|' — the field separator — despite the data being full of them.
    EXPECT_EQ(std::count(s.begin(), s.end(), '|'), 1);

    VersionedValue back = VersionedValue::deserialize(s);
    EXPECT_EQ(back.data, "a|b|c\nrow2");
    EXPECT_TRUE(back.clock.empty());
}

TEST(VersionedValue, EmptyData) {
    VersionedValue vv;
    vv.clock.set("n", 5);
    VersionedValue back = VersionedValue::deserialize(vv.serialize());
    EXPECT_EQ(back.data, "");
    EXPECT_EQ(back.clock.get("n"), 5u);
}

TEST(VersionedValue, TombstoneRoundTrips) {
    VersionedValue vv;
    vv.deleted = true;
    vv.clock.set("node1", 3);

    VersionedValue back = VersionedValue::deserialize(vv.serialize());
    EXPECT_TRUE(back.deleted);
    EXPECT_EQ(back.data, "");
    EXPECT_EQ(back.clock.get("node1"), 3u);
}

TEST(VersionedValue, LiveValueIsNotDeletedAndStaysTwoField) {
    // A live value must serialize byte-identically to the pre-tombstone format
    // (exactly one '|'), so nothing already on disk changes meaning.
    VersionedValue vv;
    vv.data = "payload";
    vv.clock.set("node1", 1);

    std::string s = vv.serialize();
    EXPECT_EQ(std::count(s.begin(), s.end(), '|'), 1);
    EXPECT_FALSE(VersionedValue::deserialize(s).deleted);
}

TEST(VersionedValue, TombstonePreservesPipeHeavyClockBoundary) {
    // Even when data is pipe-laden, the tombstone marker is the *last* field and
    // the clock in the middle is recovered intact.
    VersionedValue vv;
    vv.data = "a|b|c";
    vv.deleted = true;
    vv.clock.set("node2", 7);

    VersionedValue back = VersionedValue::deserialize(vv.serialize());
    EXPECT_EQ(back.data, "a|b|c");
    EXPECT_TRUE(back.deleted);
    EXPECT_EQ(back.clock.get("node2"), 7u);
}
