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

// Is `id` usable as a node id on the wire?
//
// Node ids are embedded, unescaped, in three nested text formats: verb fields
// split on '|', gossip events split on ';', and event fields split on ':' (which
// is also the host:port separator, and ',' is the SEED_NODES separator). An id
// containing any of those characters doesn't just misparse locally — it rides the
// piggyback stream and corrupts event parsing on EVERY node, fabricating phantom
// members that get probed, suspected, and re-disseminated cluster-wide. One
// operator typo in NODE_ID is enough; no attacker required.
//
// Allowlist rather than denylist: [A-Za-z0-9._-], 1..64 chars. Everything Docker
// and Swarm generate ("node1", "kvstore-3", host-style names) fits; anything that
// could ever act as a delimiter cannot. Enforced at every ingress: NODE_ID at
// startup (fatal), the SWIM_JOIN handshake (refused), and relayed gossip events
// (dropped) — see docs/decisions/tier-4.7.md.
inline bool isValidNodeId(const std::string &id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}
