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
