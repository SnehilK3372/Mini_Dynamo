#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "util.h"

// Golden vectors for hash64. These are the CROSS-LANGUAGE CONTRACT: the Java
// gateway's HashUtilTest asserts the exact same (key -> value) pairs, proving the
// C++ ring and the gateway's client-side ring agree on every position. If you
// change hash64, regenerate both tables together — and note it reshuffles the
// ring (see the comment in src/util.h).
namespace {
struct Vec {
    const char *key;
    uint64_t expected;
};
const Vec kVectors[] = {
    {"", 0xd725c98e9509182eULL},
    {"a", 0x43a941547f0150e5ULL},
    {"hello", 0xdfd47fc080e72addULL},
    {"key1", 0xaff516604481c0c3ULL},
    {"user:42", 0x673075b2a3d34704ULL},
    {"node1#vn0", 0xf057204310625a10ULL},
    {"node2#vn7", 0x005508a75d91c2baULL},
    {"node3#vn127", 0x6ec2f14861562723ULL},
    {"the quick brown fox", 0x0a7e28780b4036a3ULL},
    {"\xF0\x9F\x98\x80", 0xd3ef94a02719a21bULL},  // U+1F600 (😀) as UTF-8
};
}  // namespace

TEST(Hash64, MatchesGoldenVectors) {
    for (const auto &v : kVectors) {
        EXPECT_EQ(hash64(v.key), v.expected) << "key=\"" << v.key << "\"";
    }
}

TEST(Hash64, IsDeterministic) { EXPECT_EQ(hash64("repeat-me"), hash64("repeat-me")); }

TEST(Hash64, DistinctKeysDiffer) {
    // Not a guarantee in general, but these fixed keys must not collide.
    EXPECT_NE(hash64("key1"), hash64("key2"));
    EXPECT_NE(hash64("node1#vn0"), hash64("node1#vn1"));
}
