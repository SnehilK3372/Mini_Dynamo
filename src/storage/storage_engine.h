#pragma once

#include <functional>
#include <optional>
#include <string>

// Abstract storage seam between Node (coordination logic) and whatever holds
// the bytes. Today the only implementation is an in-memory map; Tier 1A swaps
// in RocksDB behind this same interface so the coordinator code never changes.
//
// Design notes (why the interface is this small):
//  - Values are opaque strings. Tier 1A's VersionedValue {data, vector clock}
//    will be serialized into the value string, so the engine stays a dumb
//    byte store and versioning logic lives above it where it belongs.
//  - forEach() instead of exposing an iterator type: RocksDB's iterator and a
//    map's iterator have nothing in common, but both can drive a visitor
//    callback. This keeps implementations trivial at the cost of not being
//    able to pause mid-scan — acceptable until anti-entropy needs more.
//  - Implementations own their thread safety. Callers (Node handlers run on
//    one detached thread per connection) may call concurrently.
class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    // Store or overwrite key -> value.
    virtual void put(const std::string &key, const std::string &value) = 0;

    // Return the value, or nullopt if the key is absent.
    virtual std::optional<std::string> get(const std::string &key) = 0;

    // Visit every key/value pair. Order is implementation-defined.
    virtual void forEach(
        const std::function<void(const std::string &key, const std::string &value)> &fn) = 0;
};
