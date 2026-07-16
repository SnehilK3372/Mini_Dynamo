package com.minidynamo.gateway.cluster;

import java.nio.charset.StandardCharsets;

/**
 * Java port of the C++ ring hash in {@code src/util.h}. Must stay bit-identical
 * to the C++ {@code hash64} so the gateway's client-side ring ({@link RingRouter})
 * places keys and vnodes on exactly the same positions the cluster nodes do —
 * that is what lets the gateway route straight to a key's primary owner.
 *
 * <p>The contract is pinned by golden vectors shared with {@code tests/hash_test.cpp}
 * (see {@code HashUtilTest}). The algorithm is FNV-1a over the UTF-8 bytes followed
 * by the MurmurHash3 fmix64 finalizer. Java's {@code long} is signed, but every
 * operation here (xor, wrapping multiply, unsigned right shift {@code >>>}) matches
 * the C++ {@code uint64_t} bit-for-bit.
 */
public final class HashUtil {

    private static final long FNV_OFFSET_BASIS = 1469598103934665603L;  // == 0xcbf29ce484222325
    private static final long FNV_PRIME = 1099511628211L;               // == 0x100000001b3

    private HashUtil() {}

    public static long hash64(String s) {
        long h = FNV_OFFSET_BASIS;
        for (byte b : s.getBytes(StandardCharsets.UTF_8)) {
            h ^= (b & 0xffL);  // treat the byte as unsigned, like C++ `unsigned char`
            h *= FNV_PRIME;    // wraps mod 2^64, matching uint64_t
        }
        // MurmurHash3 fmix64 finalizer — `>>>` is the unsigned shift matching C++ `>>`.
        h ^= (h >>> 33);
        h *= 0xff51afd7ed558ccdL;
        h ^= (h >>> 33);
        h *= 0xc4ceb9fe1a85ec53L;
        h ^= (h >>> 33);
        return h;
    }
}
