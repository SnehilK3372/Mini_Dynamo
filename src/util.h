// util.h
#pragma once
#include <cstdint>
#include <functional>
#include <string>

// 64-bit hash wrapper
inline uint64_t hash64(const std::string &s) {
    static std::hash<std::string> hasher;
    uint64_t h = (uint64_t)hasher(s);
    h ^= (h >> 33);
    h *= 0xff51afd7ed558ccdULL;
    h ^= (h >> 33);
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= (h >> 33);
    return h;
}
