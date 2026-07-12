#include "hints/hint_store.h"

#include <chrono>
#include <thread>

#include "gtest/gtest.h"
#include "vector_clock.h"
#include "versioned_value.h"

using namespace std;
using namespace chrono;

TEST(HintStoreTest, StoreAndDrain) {
    HintStore store(seconds(3600), 1000);

    VersionedValue vv{"hello", VectorClock(), false};
    store.store("node2", "key1", vv);
    store.store("node2", "key2", vv);
    store.store("node3", "key3", vv);

    EXPECT_EQ(store.hintCount(), 3u);
    EXPECT_EQ(store.hintCountFor("node2"), 2u);
    EXPECT_EQ(store.hintCountFor("node3"), 1u);

    auto drained = store.drain("node2");
    EXPECT_EQ(drained.size(), 2u);
    EXPECT_EQ(drained[0].key, "key1");
    EXPECT_EQ(drained[1].key, "key2");
    EXPECT_EQ(store.hintCountFor("node2"), 0u);
    EXPECT_EQ(store.hintCount(), 1u);
}

TEST(HintStoreTest, CapEnforced) {
    HintStore store(seconds(3600), 3);

    VersionedValue vv{"v", VectorClock(), false};
    store.store("node2", "k1", vv);
    store.store("node2", "k2", vv);
    store.store("node2", "k3", vv);
    store.store("node2", "k4", vv);  // exceeds cap

    EXPECT_EQ(store.hintCountFor("node2"), 3u);
}

TEST(HintStoreTest, TTLExpiry) {
    // TTL of 1 second for test speed.
    HintStore store(seconds(1), 1000);

    VersionedValue vv{"v", VectorClock(), false};
    store.store("node2", "k1", vv);
    EXPECT_EQ(store.hintCount(), 1u);

    this_thread::sleep_for(milliseconds(1200));
    store.expireOld();

    EXPECT_EQ(store.hintCount(), 0u);
}

TEST(HintStoreTest, DrainEmptyTarget) {
    HintStore store;
    auto drained = store.drain("nonexistent");
    EXPECT_TRUE(drained.empty());
}

TEST(HintStoreTest, HintPreservesVectorClock) {
    HintStore store;

    VectorClock vc;
    vc.set("node1", 5);
    vc.set("node2", 3);
    VersionedValue vv{"data", vc, false};

    store.store("node3", "mykey", vv);
    auto drained = store.drain("node3");
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].value.data, "data");
    EXPECT_EQ(drained[0].value.clock.get("node1"), 5u);
    EXPECT_EQ(drained[0].value.clock.get("node2"), 3u);
}
