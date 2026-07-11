#ifdef HAVE_ROCKSDB

#include "rocksdb_storage.h"

#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <stdexcept>

using namespace std;

RocksDBStorageEngine::RocksDBStorageEngine(const string &path) {
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::DB *raw = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(options, path, &raw);
    if (!s.ok()) {
        throw runtime_error("RocksDB open failed at '" + path + "': " + s.ToString());
    }
    db_.reset(raw);
}

// Defined here (not defaulted in the header) so unique_ptr sees the complete
// rocksdb::DB type at destruction.
RocksDBStorageEngine::~RocksDBStorageEngine() = default;

void RocksDBStorageEngine::put(const string &key, const string &value) {
    // WriteOptions default: the write goes to the memtable and WAL. The WAL is
    // what makes it survive a crash/restart — the whole point of using RocksDB.
    db_->Put(rocksdb::WriteOptions(), key, value);
}

optional<string> RocksDBStorageEngine::get(const string &key) {
    string value;
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &value);
    if (s.ok()) return value;
    return nullopt;  // NotFound (or any read error) → absent
}

void RocksDBStorageEngine::forEach(
    const function<void(const string &, const string &)> &fn) {
    unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions()));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        fn(it->key().ToString(), it->value().ToString());
    }
}

#endif  // HAVE_ROCKSDB
