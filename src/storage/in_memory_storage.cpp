#include "in_memory_storage.h"

using namespace std;

void InMemoryStorageEngine::put(const string &key, const string &value){
    lock_guard<mutex> lock(mtx);
    data[key] = value;
}

optional<string> InMemoryStorageEngine::get(const string &key){
    lock_guard<mutex> lock(mtx);
    auto it = data.find(key);
    if (it != data.end()){
        return it->second;
    }
    return nullopt;
}

void InMemoryStorageEngine::forEach(
    const function<void(const string &, const string &)> &fn){
    // Holds the lock for the whole scan — fine at current scale; revisit if a
    // future anti-entropy pass ever iterates a large store while writes wait.
    lock_guard<mutex> lock(mtx);
    for (const auto &kv : data){
        fn(kv.first, kv.second);
    }
}
