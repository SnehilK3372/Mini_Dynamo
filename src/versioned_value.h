#pragma once

#include <string>

#include "vector_clock.h"

// A value together with the vector clock that versions it. This is what actually
// lives in the storage engine and travels on the wire: the store stays a dumb
// byte map (it never learns about clocks), and all versioning logic lives above
// it — exactly the separation storage_engine.h was designed for in Tier 0.
struct VersionedValue {
    std::string data;   // opaque user bytes (may contain '|', NUL, newlines)
    VectorClock clock;

    // Storage form: a single self-contained string handed to StorageEngine::put.
    // Layout: "<base64(data)>|<clock-token>". base64 never emits '|' and the
    // clock token never contains '|', so the single '|' split is unambiguous even
    // when the underlying data does contain pipes.
    std::string serialize() const;
    static VersionedValue deserialize(const std::string &stored);
};
