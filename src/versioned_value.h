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
    bool deleted = false;  // tombstone: a delete is a versioned value like any other,
                           // so it propagates by quorum + read repair (Dynamo-style),
                           // not a silent drop. `data` is empty for a tombstone.

    // Storage form: a single self-contained string handed to StorageEngine::put.
    // Layout: "<base64(data)>|<clock-token>" for a live value, with an extra
    // "|D" appended for a tombstone. base64 never emits '|' and the clock token
    // never contains '|', so the split stays unambiguous even when the underlying
    // data contains pipes. The two-field (and bare) forms written before
    // tombstones existed still parse — deleted defaults to false.
    std::string serialize() const;
    static VersionedValue deserialize(const std::string &stored);
};
