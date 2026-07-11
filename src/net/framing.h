#pragma once

#include <cstddef>
#include <string>

// Length-prefixed message framing over a socket fd.
//
// Tier 1A replaces the old "one read() of up to 4096 bytes" transport, which
// silently truncated any message that arrived in more than one TCP segment or
// exceeded the buffer. That was survivable while values were tiny bare strings;
// it is not once a response can carry a value plus a vector clock, or several
// concurrent siblings. Every message is now framed as:
//
//     <decimal-length>\n<exactly that many payload bytes>
//
// The text length line keeps frames debuggable and the payload stays the existing
// pipe-delimited format. recv loops until the whole frame is present.
namespace framing {

// Upper bound on a single frame, so a malformed or hostile length can't make us
// allocate unbounded memory.
constexpr size_t kMaxFrame = 16 * 1024 * 1024;

// Writes the full framed payload. Returns false on any short/failed write.
bool sendFramed(int fd, const std::string &payload);

// Reads one whole frame into `out`. Returns false on EOF, error, malformed
// header, or a length exceeding kMaxFrame.
bool recvFramed(int fd, std::string &out);

}  // namespace framing
