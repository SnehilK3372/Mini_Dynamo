#include "base64.h"

#include <array>
#include <cstdint>

namespace base64 {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Reverse lookup: maps an encoded char back to its 6-bit value, 0xFF for
// anything that is not part of the alphabet (padding, whitespace, garbage).
std::array<uint8_t, 256> makeDecodeTable() {
    std::array<uint8_t, 256> t{};
    t.fill(0xFF);
    for (uint8_t i = 0; i < 64; ++i) {
        t[static_cast<uint8_t>(kAlphabet[i])] = i;
    }
    return t;
}
const std::array<uint8_t, 256> kDecode = makeDecodeTable();
}  // namespace

std::string encode(const std::string &raw) {
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 3 <= raw.size(); i += 3) {
        uint32_t n = (static_cast<uint8_t>(raw[i]) << 16) |
                     (static_cast<uint8_t>(raw[i + 1]) << 8) | (static_cast<uint8_t>(raw[i + 2]));
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back(kAlphabet[n & 0x3F]);
    }

    // Tail: 1 or 2 leftover bytes get padded with '='.
    size_t rem = raw.size() - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint8_t>(raw[i]) << 16;
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint8_t>(raw[i]) << 16) | (static_cast<uint8_t>(raw[i + 1]) << 8);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(kAlphabet[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string decode(const std::string &encoded) {
    std::string out;
    out.reserve(encoded.size() / 4 * 3);

    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char c : encoded) {
        uint8_t v = kDecode[c];
        if (v == 0xFF) continue;  // skip padding and any stray characters
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace base64
