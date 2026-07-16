// util.h
#pragma once
#include <cstdint>
#include <string>

// 64-bit ring hash. Fully specified and byte-level so it is reproducible in
// other languages (the Java gateway ports this exactly for ring-aware routing,
// Tier 4.4). The seed is FNV-1a over the raw UTF-8 bytes; the tail is the
// MurmurHash3 fmix64 finalizer for good avalanche/distribution on the ring.
//
// NOTE: this replaced an earlier version seeded from std::hash<std::string>,
// which is implementation-defined and could not be reproduced cross-language.
// Changing the hash reshuffles every key's ring position, so a cluster must run
// a single build of this function — deploy all nodes at once, on a fresh ring.
inline uint64_t hash64(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64-bit offset basis
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;  // FNV-1a 64-bit prime
    }
    // MurmurHash3 fmix64 finalizer.
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= (h >> 33);
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= (h >> 33);
    return h;
}
