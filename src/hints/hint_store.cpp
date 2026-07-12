#include "hint_store.h"

#include <algorithm>

HintStore::HintStore(std::chrono::seconds ttl, size_t max_hints_per_target)
    : ttl_(ttl), max_per_target_(max_hints_per_target) {}

void HintStore::store(const std::string &target_node_id, const std::string &key,
                      const VersionedValue &value) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &list = hints_[target_node_id];
    if (list.size() >= max_per_target_) return;  // cap reached, drop hint
    list.push_back({target_node_id, key, value, std::chrono::steady_clock::now()});
}

std::vector<Hint> HintStore::drain(const std::string &target_node_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = hints_.find(target_node_id);
    if (it == hints_.end()) return {};
    std::vector<Hint> out = std::move(it->second);
    hints_.erase(it);
    return out;
}

void HintStore::expireOld() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = hints_.begin(); it != hints_.end();) {
        auto &list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const Hint &h) { return (now - h.created_at) > ttl_; }),
                   list.end());
        if (list.empty()) {
            it = hints_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t HintStore::hintCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    size_t total = 0;
    for (const auto &[id, list] : hints_) {
        total += list.size();
    }
    return total;
}

size_t HintStore::hintCountFor(const std::string &target_node_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = hints_.find(target_node_id);
    if (it == hints_.end()) return 0;
    return it->second.size();
}
