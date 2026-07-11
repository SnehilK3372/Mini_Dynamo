#pragma once

#include <cstdint>
#include <map>
#include <string>

// A vector clock: a map from node id to a monotonically increasing counter that
// records "how many writes originating at each node this version has causally
// seen." It is the mechanism that lets Mini Dynamo *detect* conflicts instead of
// silently overwriting — the intellectual heart of Tier 1A.
//
// compare() is the whole point: given two versions it tells you whether one is a
// causal descendant of the other (dominates → safe to supersede) or whether they
// are concurrent (a genuine conflict from writes that didn't see each other →
// keep both as siblings). This is why we use vector clocks rather than
// last-write-wins timestamps, which cannot even tell that a conflict happened.
//
// std::map (ordered) is deliberate: it gives a canonical key order for free, so
// serialize() is deterministic and two equal clocks always produce the same
// string (important for equality checks and stable wire output). Zero-valued
// entries are never stored, so an absent node and an explicit 0 are the same
// thing — get() returns 0 for anything not present.
class VectorClock {
   public:
    enum class Ordering {
        EQUAL,        // identical causal history
        A_DOMINATES,  // a is a strict causal descendant of b (a supersedes b)
        B_DOMINATES,  // b is a strict causal descendant of a
        CONCURRENT    // neither dominates — a real conflict
    };

    uint64_t get(const std::string &node) const;
    void set(const std::string &node, uint64_t counter);
    void increment(const std::string &node);  // += 1

    const std::map<std::string, uint64_t> &entries() const { return counts_; }
    bool empty() const { return counts_.empty(); }

    // Pairwise comparison. See Ordering. O(|a| + |b|).
    static Ordering compare(const VectorClock &a, const VectorClock &b);

    // Element-wise max of two clocks. Used to build a clock that has seen
    // everything both inputs saw (e.g. when reconciling context on a write).
    static VectorClock merge(const VectorClock &a, const VectorClock &b);

    // Canonical form: "node1:3,node2:1" with keys in sorted order; empty clock
    // serializes to the empty string. Node ids must not contain ':' or ','
    // (true for the "nodeN" ids this project uses); this keeps the token safe to
    // embed as a single pipe-delimited field on the wire.
    std::string serialize() const;
    static VectorClock parse(const std::string &s);

    bool operator==(const VectorClock &other) const { return counts_ == other.counts_; }
    bool operator!=(const VectorClock &other) const { return !(*this == other); }

   private:
    std::map<std::string, uint64_t> counts_;
};
