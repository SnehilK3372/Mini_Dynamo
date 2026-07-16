package com.minidynamo.gateway.cluster;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.jupiter.api.Test;

/**
 * Cross-language contract test: these golden (key -> value) pairs are the EXACT
 * same table as {@code tests/hash_test.cpp}. If both this test and the C++
 * hash_test pass, the gateway's ring and the cluster's ring agree on every
 * position. The hex literals are unsigned 64-bit values; as Java {@code long}
 * some are negative, but the bit patterns match the C++ {@code uint64_t}.
 */
class HashUtilTest {

    @Test
    void matchesGoldenVectors() {
        assertThat(HashUtil.hash64("")).isEqualTo(0xd725c98e9509182eL);
        assertThat(HashUtil.hash64("a")).isEqualTo(0x43a941547f0150e5L);
        assertThat(HashUtil.hash64("hello")).isEqualTo(0xdfd47fc080e72addL);
        assertThat(HashUtil.hash64("key1")).isEqualTo(0xaff516604481c0c3L);
        assertThat(HashUtil.hash64("user:42")).isEqualTo(0x673075b2a3d34704L);
        assertThat(HashUtil.hash64("node1#vn0")).isEqualTo(0xf057204310625a10L);
        assertThat(HashUtil.hash64("node2#vn7")).isEqualTo(0x005508a75d91c2baL);
        assertThat(HashUtil.hash64("node3#vn127")).isEqualTo(0x6ec2f14861562723L);
        assertThat(HashUtil.hash64("the quick brown fox")).isEqualTo(0x0a7e28780b4036a3L);
        assertThat(HashUtil.hash64("😀")).isEqualTo(0xd3ef94a02719a21bL);  // U+1F600 😀
    }

    @Test
    void isDeterministic() {
        assertThat(HashUtil.hash64("repeat-me")).isEqualTo(HashUtil.hash64("repeat-me"));
    }
}
