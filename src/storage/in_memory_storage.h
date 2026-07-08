#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "storage_engine.h"

// The pre-Tier-1A engine: a mutex-guarded hash map. Not durable — a restart
// loses everything — which is exactly the gap RocksDB closes later. Kept
// around permanently afterwards as the fast, dependency-free engine for tests.
class InMemoryStorageEngine : public StorageEngine {
public:
    void put(const std::string &key, const std::string &value) override;
    std::optional<std::string> get(const std::string &key) override;
    void forEach(
        const std::function<void(const std::string &key, const std::string &value)> &fn) override;

private:
    // The engine owns its lock so thread safety is part of the StorageEngine
    // contract rather than something every caller has to remember.
    std::mutex mtx;
    std::unordered_map<std::string, std::string> data;
};
