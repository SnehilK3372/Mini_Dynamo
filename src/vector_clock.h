#pragma once

#include <cstddef>
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
//
// Tier 4.5 adds a per-entry timestamp so the clock can be *pruned* (see prune()):
// at thousands of nodes an unbounded clock would grow one entry per coordinator
// that ever touched the key, bloating every value, message, and comparison. The
// timestamp is pruning metadata ONLY — it never participates in causality
// (compare() and operator== look at counters alone).
class VectorClock {
   public:
    enum class Ordering {
        EQUAL,        // identical causal history
        A_DOMINATES,  // a is a strict causal descendant of b (a supersedes b)
        B_DOMINATES,  // b is a strict causal descendant of a
        CONCURRENT    // neither dominates — a real conflict
    };

    // One node's contribution: the counter (causal information) plus when that
    // counter last moved, in epoch milliseconds. The timestamp travels on the
    // wire so that every node prunes a given clock identically — a local-only
    // timestamp would make different replicas drop different entries and
    // manufacture avoidable conflicts.
    struct Entry {
        uint64_t counter = 0;
        uint64_t updated_ms = 0;
    };

    uint64_t get(const std::string &node) const;

    // Set the counter, stamping updated_ms with the current wall-clock time.
    void set(const std::string &node, uint64_t counter);
    // Set the counter with an explicit timestamp (used by parse(), so a clock
    // read off the wire keeps the timestamps it was written with).
    void set(const std::string &node, uint64_t counter, uint64_t updated_ms);

    void increment(const std::string &node);  // += 1, restamps updated_ms

    const std::map<std::string, Entry> &entries() const { return counts_; }
    bool empty() const { return counts_.empty(); }
    std::size_t size() const { return counts_.size(); }

    // Bound the clock: drop entries with the oldest updated_ms until at most
    // max_entries remain. No-op when already within the bound.
    //
    // Deterministic — ties on updated_ms are broken by node id, so two nodes
    // pruning an identical clock produce an identical result and don't diverge.
    //
    // Pruning is lossy by nature: dropping an entry discards causal information.
    // The common outcome is a conservative one — two versions that were really
    // ordered may compare CONCURRENT and surface as siblings. It is not a strict
    // guarantee (see docs/decisions/tier-4.5.md); max_entries is set generously
    // so the pathological case stays out of reach. Dynamo makes the same trade.
    void prune(std::size_t max_entries);

    // Pairwise comparison on counters only. See Ordering. O(|a| + |b|).
    static Ordering compare(const VectorClock &a, const VectorClock &b);

    // Element-wise max of two clocks (by counter); the winning entry's timestamp
    // rides along. Used to build a clock that has seen everything both inputs saw.
    static VectorClock merge(const VectorClock &a, const VectorClock &b);

    // Canonical form: "node1:3:1720000000000,node2:1:1720000000001" with keys in
    // sorted order; empty clock serializes to the empty string. Node ids must not
    // contain ':' or ',' (true for the "nodeN" ids this project uses); this keeps
    // the token safe to embed as a single pipe-delimited field on the wire.
    //
    // Tier 4.5 changed this from "node:counter" to "node:counter:timestamp" — a
    // breaking wire/storage change. parse() still accepts the 2-field legacy form
    // (timestamp 0 → treated as oldest), so a stray old value degrades instead of
    // failing, but a cluster must run a single build.
    std::string serialize() const;
    static VectorClock parse(const std::string &s);

    // Causal equality: counters only. Two clocks with the same history are equal
    // even if their entries were stamped at different times.
    bool operator==(const VectorClock &other) const;
    bool operator!=(const VectorClock &other) const { return !(*this == other); }

   private:
    std::map<std::string, Entry> counts_;
};
