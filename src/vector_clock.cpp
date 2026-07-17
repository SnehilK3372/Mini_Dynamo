#include "vector_clock.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>
#include <vector>

using namespace std;

namespace {
// Wall-clock epoch millis. system_clock (not steady_clock) on purpose: these
// timestamps are serialized and compared *across nodes*, so they must share an
// epoch. Modest clock skew between hosts only perturbs which entry looks oldest
// — it can never affect causality, which is counter-based.
uint64_t nowMs() {
    return static_cast<uint64_t>(
        chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

uint64_t VectorClock::get(const string &node) const {
    auto it = counts_.find(node);
    return it == counts_.end() ? 0 : it->second.counter;
}

void VectorClock::set(const string &node, uint64_t counter) { set(node, counter, nowMs()); }

void VectorClock::set(const string &node, uint64_t counter, uint64_t updated_ms) {
    // Never store zeros: an explicit 0 is indistinguishable from "absent", and
    // keeping it out preserves the invariant that equal histories compare equal.
    if (counter == 0) {
        counts_.erase(node);
    } else {
        counts_[node] = Entry{counter, updated_ms};
    }
}

void VectorClock::increment(const string &node) {
    Entry &e = counts_[node];
    e.counter += 1;
    e.updated_ms = nowMs();
}

void VectorClock::prune(size_t max_entries) {
    if (counts_.size() <= max_entries) return;

    // Order by (oldest first, then node id). The node-id tiebreak is what makes
    // pruning deterministic: two nodes handed the same clock drop the same
    // entries, so pruning never causes replicas to diverge on its own.
    vector<pair<uint64_t, string>> by_age;
    by_age.reserve(counts_.size());
    for (const auto &kv : counts_) by_age.emplace_back(kv.second.updated_ms, kv.first);
    sort(by_age.begin(), by_age.end());

    size_t to_drop = counts_.size() - max_entries;
    for (size_t i = 0; i < to_drop; ++i) counts_.erase(by_age[i].second);
}

VectorClock::Ordering VectorClock::compare(const VectorClock &a, const VectorClock &b) {
    bool aGreaterSomewhere = false;
    bool bGreaterSomewhere = false;

    // Counters only — timestamps are pruning metadata and carry no causality.
    // Walk the union of keys. Because both maps are sorted, we could merge-walk,
    // but N is bounded (prune() caps it), so two straight lookups per side reads
    // clearer and costs nothing that matters.
    for (const auto &kv : a.counts_) {
        uint64_t bv = b.get(kv.first);
        if (kv.second.counter > bv)
            aGreaterSomewhere = true;
        else if (bv > kv.second.counter)
            bGreaterSomewhere = true;
    }
    for (const auto &kv : b.counts_) {
        if (a.counts_.find(kv.first) != a.counts_.end()) continue;  // already counted
        // a has 0 here, b has kv.second.counter > 0
        bGreaterSomewhere = true;
    }

    if (!aGreaterSomewhere && !bGreaterSomewhere) return Ordering::EQUAL;
    if (aGreaterSomewhere && !bGreaterSomewhere) return Ordering::A_DOMINATES;
    if (bGreaterSomewhere && !aGreaterSomewhere) return Ordering::B_DOMINATES;
    return Ordering::CONCURRENT;
}

VectorClock VectorClock::merge(const VectorClock &a, const VectorClock &b) {
    VectorClock out = a;
    for (const auto &kv : b.counts_) {
        // Whichever side's counter wins contributes its timestamp too, so the
        // merged entry's age reflects the write it actually came from.
        if (kv.second.counter > out.get(kv.first)) out.counts_[kv.first] = kv.second;
    }
    return out;
}

bool VectorClock::operator==(const VectorClock &other) const {
    // Causal equality: counters only. Timestamps are deliberately ignored so a
    // clock that round-tripped through a peer (restamped, same history) still
    // equals the original.
    if (counts_.size() != other.counts_.size()) return false;
    for (const auto &kv : counts_) {
        auto it = other.counts_.find(kv.first);
        if (it == other.counts_.end() || it->second.counter != kv.second.counter) return false;
    }
    return true;
}

string VectorClock::serialize() const {
    ostringstream oss;
    bool first = true;
    for (const auto &kv : counts_) {  // sorted by node id → canonical
        if (!first) oss << ',';
        oss << kv.first << ':' << kv.second.counter << ':' << kv.second.updated_ms;
        first = false;
    }
    return oss.str();
}

VectorClock VectorClock::parse(const string &s) {
    VectorClock vc;
    if (s.empty()) return vc;

    stringstream ss(s);
    string entry;
    while (getline(ss, entry, ',')) {
        if (entry.empty()) continue;
        // "node:counter:ts" — node ids contain no ':', so the FIRST colon ends the
        // id and everything after is the numeric tail.
        auto first_colon = entry.find(':');
        if (first_colon == string::npos) continue;  // malformed entry, skip defensively
        string node = entry.substr(0, first_colon);
        string tail = entry.substr(first_colon + 1);
        if (node.empty() || tail.empty()) continue;

        string num = tail;
        string ts = "0";  // legacy "node:counter" → oldest, so it prunes first
        auto second_colon = tail.find(':');
        if (second_colon != string::npos) {
            num = tail.substr(0, second_colon);
            ts = tail.substr(second_colon + 1);
            if (ts.empty()) ts = "0";
        }
        if (num.empty()) continue;
        try {
            vc.set(node, static_cast<uint64_t>(stoull(num)), static_cast<uint64_t>(stoull(ts)));
        } catch (...) {
            // ignore malformed counter/timestamp
        }
    }
    return vc;
}
