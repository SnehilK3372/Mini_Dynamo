// Runtime-verified in Docker/CI (this dev machine has no RocksDB). Compiled only
// when the build found RocksDB; otherwise this translation unit is empty.
#ifdef HAVE_ROCKSDB

#include "storage/rocksdb_storage.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "versioned_value.h"

namespace {
// A unique-ish scratch directory per test process.
std::string tmpDir(const std::string &name) {
    return std::string("/tmp/minidynamo_rocks_") + name + "_" + std::to_string(::getpid());
}
}  // namespace

TEST(RocksDBStorage, ValuesSurviveReopen) {
    std::string dir = tmpDir("persist");
    {
        RocksDBStorageEngine db(dir);
        db.put("k", "durable-value");
    }  // closed — simulates a node stopping
    {
        RocksDBStorageEngine db(dir);  // reopened — simulates restart
        auto v = db.get("k");
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, "durable-value");
    }
}

TEST(RocksDBStorage, StoresSerializedVersionedValue) {
    std::string dir = tmpDir("vv");
    VersionedValue vv{"hello", {}};
    vv.clock.set("node1", 3);
    {
        RocksDBStorageEngine db(dir);
        db.put("k", vv.serialize());
    }
    RocksDBStorageEngine db(dir);
    auto stored = db.get("k");
    ASSERT_TRUE(stored.has_value());
    VersionedValue back = VersionedValue::deserialize(*stored);
    EXPECT_EQ(back.data, "hello");
    EXPECT_EQ(back.clock.get("node1"), 3u);
}

TEST(RocksDBStorage, TwoNodesStorageAreIndependent) {
    RocksDBStorageEngine a(tmpDir("a"));
    RocksDBStorageEngine b(tmpDir("b"));
    a.put("k", "value-a");
    // b never saw the write — shared-nothing.
    EXPECT_FALSE(b.get("k").has_value());
    b.put("k", "value-b");
    EXPECT_EQ(*a.get("k"), "value-a");
    EXPECT_EQ(*b.get("k"), "value-b");
}

#endif  // HAVE_ROCKSDB
