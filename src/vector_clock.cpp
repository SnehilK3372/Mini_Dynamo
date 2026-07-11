#include "vector_clock.h"

#include <sstream>

using namespace std;

uint64_t VectorClock::get(const string &node) const {
    auto it = counts_.find(node);
    return it == counts_.end() ? 0 : it->second;
}

void VectorClock::set(const string &node, uint64_t counter) {
    // Never store zeros: an explicit 0 is indistinguishable from "absent", and
    // keeping it out preserves the invariant that equal histories compare equal.
    if (counter == 0) {
        counts_.erase(node);
    } else {
        counts_[node] = counter;
    }
}

void VectorClock::increment(const string &node) { counts_[node] += 1; }

VectorClock::Ordering VectorClock::compare(const VectorClock &a, const VectorClock &b) {
    bool aGreaterSomewhere = false;
    bool bGreaterSomewhere = false;

    // Walk the union of keys. Because both maps are sorted, we could merge-walk,
    // but N is tiny (one entry per node that ever coordinated this key), so two
    // straight lookups per side reads clearer and costs nothing that matters.
    for (const auto &kv : a.counts_) {
        uint64_t bv = b.get(kv.first);
        if (kv.second > bv)
            aGreaterSomewhere = true;
        else if (bv > kv.second)
            bGreaterSomewhere = true;
    }
    for (const auto &kv : b.counts_) {
        if (a.counts_.find(kv.first) != a.counts_.end()) continue;  // already counted
        // a has 0 here, b has kv.second > 0
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
        if (kv.second > out.get(kv.first)) out.counts_[kv.first] = kv.second;
    }
    return out;
}

string VectorClock::serialize() const {
    ostringstream oss;
    bool first = true;
    for (const auto &kv : counts_) {  // sorted by node id → canonical
        if (!first) oss << ',';
        oss << kv.first << ':' << kv.second;
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
        auto colon = entry.rfind(':');
        if (colon == string::npos) continue;  // malformed entry, skip defensively
        string node = entry.substr(0, colon);
        string num = entry.substr(colon + 1);
        if (node.empty() || num.empty()) continue;
        try {
            vc.set(node, static_cast<uint64_t>(stoull(num)));
        } catch (...) {
            // ignore malformed counter
        }
    }
    return vc;
}
