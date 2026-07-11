#pragma once

#ifdef HAVE_ROCKSDB

#include <memory>
#include <string>

#include "storage_engine.h"

namespace rocksdb {
class DB;
}

// Durable, per-node storage backed by RocksDB (an embedded LSM-tree engine).
// Each node owns its own directory on its own disk — shared-nothing, exactly
// like Dynamo: nodes coordinate only over the network, never through shared
// storage. Values are opaque serialized VersionedValue strings; RocksDB never
// learns about vector clocks, keeping versioning above the byte store where it
// belongs.
//
// Compiled only when the build found RocksDB (HAVE_ROCKSDB). The in-memory
// engine remains the fallback and the test engine.
class RocksDBStorageEngine : public StorageEngine {
public:
    // Opens (creating if needed) a RocksDB database at `path`. Throws
    // std::runtime_error if the database cannot be opened.
    explicit RocksDBStorageEngine(const std::string &path);
    ~RocksDBStorageEngine() override;

    void put(const std::string &key, const std::string &value) override;
    std::optional<std::string> get(const std::string &key) override;
    void forEach(
        const std::function<void(const std::string &key, const std::string &value)> &fn) override;

private:
    std::unique_ptr<rocksdb::DB> db_;
};

#endif  // HAVE_ROCKSDB
