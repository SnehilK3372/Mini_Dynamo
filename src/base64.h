#pragma once

#include <string>

// Base64 (standard alphabet, RFC 4648) over arbitrary bytes.
//
// Why this exists in Tier 1A: the wire protocol is pipe-delimited with no
// escaping, and a stored value is an opaque byte string that may legitimately
// contain '|', newlines, or NUL. Base64-encoding the value field before it goes
// on the wire (and into the VersionedValue serialization) keeps the delimiters
// unambiguous without inventing an escaping scheme. Keys and control tokens stay
// plaintext because they are constrained to delimiter-free characters already.
namespace base64 {

std::string encode(const std::string &raw);

// Decodes; on malformed input returns the best-effort decode of the valid
// prefix. Callers only ever feed encode()'s own output, so this is defensive.
std::string decode(const std::string &encoded);

}  // namespace base64
