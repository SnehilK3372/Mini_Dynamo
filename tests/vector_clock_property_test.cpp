#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "vector_clock.h"

// Property-based tests for the vector clock: the correctness core of the whole
// system, and pure functions, so it is the ideal place for them.
//
// Why properties and not more examples: the example tests I wrote asserted what I
// already believed. The SWIM rejoin bug slipped through 89 green tests precisely
// because its test encoded my assumption (an artificially bumped incarnation)
// rather than reality. Randomised inputs explore the cases nobody thought to
// write — including, below, an executable probe of the pruning risk that
// docs/decisions/tier-4.5.md so far only argues in prose.
//
// Hand-rolled generators rather than RapidCheck: the project deliberately keeps
// dependencies minimal (Tier 0 removed nlohmann-json). What's given up is
// automatic shrinking; clocks this small print readably as-is, and every failure
// reports its seed and the exact counterexample so it can be replayed.
using Ordering = VectorClock::Ordering;

namespace {

constexpr int kCases = 1000;
constexpr uint64_t kSeed = 0xD1AA0;  // fixed → reproducible; print on failure

// A small alphabet and counter range on purpose: with few node ids and low
// counters, random clocks actually collide, dominate and conflict. Wide ranges
// would make almost every pair CONCURRENT and test nothing.
const char *kNodes[] = {"a", "b", "c", "d", "e"};
constexpr int kNodeCount = 5;

VectorClock randomClock(std::mt19937_64 &rng, int max_entries = 5, uint64_t max_counter = 4) {
    VectorClock v;
    std::uniform_int_distribution<int> n_dist(0, max_entries);
    std::uniform_int_distribution<uint64_t> c_dist(0, max_counter);
    std::uniform_int_distribution<uint64_t> ts_dist(1000, 1050);
    int n = n_dist(rng);
    for (int i = 0; i < n; ++i) {
        // set() with counter 0 erases, which is itself part of the contract.
        v.set(kNodes[i % kNodeCount], c_dist(rng), ts_dist(rng));
    }
    return v;
}

// A clock with many entries, for pruning properties.
VectorClock randomWideClock(std::mt19937_64 &rng, int entries) {
    VectorClock v;
    std::uniform_int_distribution<uint64_t> c_dist(1, 9);
    std::uniform_int_distribution<uint64_t> ts_dist(1000, 1000 + entries * 2);
    for (int i = 0; i < entries; ++i) {
        v.set("n" + std::to_string(i), c_dist(rng), ts_dist(rng));
    }
    return v;
}

std::string why(const VectorClock &a, const VectorClock &b, int case_no) {
    return "seed=" + std::to_string(kSeed) + " case=" + std::to_string(case_no) +
           "\n  a = " + a.serialize() + "\n  b = " + b.serialize();
}

bool dominatesOrEqual(const VectorClock &a, const VectorClock &b) {
    auto o = VectorClock::compare(a, b);
    return o == Ordering::EQUAL || o == Ordering::A_DOMINATES;
}

}  // namespace

// ---- compare() is a well-formed partial order ---------------------------

TEST(VectorClockProperty, CompareIsReflexive) {
    std::mt19937_64 rng(kSeed);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng);
        ASSERT_EQ(VectorClock::compare(a, a), Ordering::EQUAL) << why(a, a, i);
    }
}

TEST(VectorClockProperty, CompareIsAntisymmetric) {
    std::mt19937_64 rng(kSeed + 1);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng), b = randomClock(rng);
        auto ab = VectorClock::compare(a, b);
        auto ba = VectorClock::compare(b, a);
        // Swapping the arguments must mirror the verdict, never contradict it.
        switch (ab) {
            case Ordering::EQUAL:
                ASSERT_EQ(ba, Ordering::EQUAL) << why(a, b, i);
                break;
            case Ordering::A_DOMINATES:
                ASSERT_EQ(ba, Ordering::B_DOMINATES) << why(a, b, i);
                break;
            case Ordering::B_DOMINATES:
                ASSERT_EQ(ba, Ordering::A_DOMINATES) << why(a, b, i);
                break;
            case Ordering::CONCURRENT:
                ASSERT_EQ(ba, Ordering::CONCURRENT) << why(a, b, i);
                break;
        }
    }
}

TEST(VectorClockProperty, DominanceIsTransitive) {
    std::mt19937_64 rng(kSeed + 2);
    int checked = 0;
    for (int i = 0; i < kCases * 5; ++i) {
        VectorClock a = randomClock(rng), b = randomClock(rng), cc = randomClock(rng);
        if (VectorClock::compare(a, b) == Ordering::A_DOMINATES &&
            VectorClock::compare(b, cc) == Ordering::A_DOMINATES) {
            ASSERT_EQ(VectorClock::compare(a, cc), Ordering::A_DOMINATES)
                << why(a, cc, i) << "\n  b = " << b.serialize();
            ++checked;
        }
    }
    EXPECT_GT(checked, 0) << "generator never produced a transitive chain — test is vacuous";
}

TEST(VectorClockProperty, EqualityAgreesWithCompare) {
    std::mt19937_64 rng(kSeed + 3);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng), b = randomClock(rng);
        EXPECT_EQ(a == b, VectorClock::compare(a, b) == Ordering::EQUAL) << why(a, b, i);
    }
}

// ---- serialization ------------------------------------------------------

TEST(VectorClockProperty, SerializeParseRoundTrips) {
    std::mt19937_64 rng(kSeed + 4);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng);
        VectorClock rt = VectorClock::parse(a.serialize());
        ASSERT_EQ(rt, a) << why(a, rt, i);
        // Canonical: serializing again yields byte-identical output.
        ASSERT_EQ(rt.serialize(), a.serialize()) << why(a, rt, i);
    }
}

// ---- merge is a least upper bound ---------------------------------------

TEST(VectorClockProperty, MergeDominatesBothInputs) {
    std::mt19937_64 rng(kSeed + 5);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng), b = randomClock(rng);
        VectorClock m = VectorClock::merge(a, b);
        // The whole point of merge: a clock that has seen everything both saw.
        ASSERT_TRUE(dominatesOrEqual(m, a)) << why(a, m, i);
        ASSERT_TRUE(dominatesOrEqual(m, b)) << why(b, m, i);
    }
}

TEST(VectorClockProperty, MergeIsCommutativeAndIdempotent) {
    std::mt19937_64 rng(kSeed + 6);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng), b = randomClock(rng);
        ASSERT_EQ(VectorClock::merge(a, b), VectorClock::merge(b, a)) << why(a, b, i);
        ASSERT_EQ(VectorClock::merge(a, a), a) << why(a, a, i);
    }
}

// ---- increment ----------------------------------------------------------

TEST(VectorClockProperty, IncrementStrictlyAdvances) {
    std::mt19937_64 rng(kSeed + 7);
    std::uniform_int_distribution<int> pick(0, kNodeCount - 1);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomClock(rng);
        VectorClock b = a;
        b.increment(kNodes[pick(rng)]);
        // A new write must always supersede what it was based on — this is what
        // stops a write being born already-dominated and silently lost.
        ASSERT_EQ(VectorClock::compare(b, a), Ordering::A_DOMINATES) << why(b, a, i);
    }
}

// ---- pruning (Tier 4.5) -------------------------------------------------

TEST(VectorClockProperty, PruneBoundsSize) {
    std::mt19937_64 rng(kSeed + 8);
    std::uniform_int_distribution<int> k_dist(0, 10);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomWideClock(rng, 25);
        size_t k = static_cast<size_t>(k_dist(rng));
        VectorClock p = a;
        p.prune(k);
        ASSERT_LE(p.size(), k) << why(a, p, i) << "\n  k = " << k;
    }
}

TEST(VectorClockProperty, PruneOnlyLosesInformation) {
    std::mt19937_64 rng(kSeed + 9);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomWideClock(rng, 25);
        VectorClock p = a;
        p.prune(20);
        // A pruned clock can only ever be a subset of its own history: the
        // original must dominate it (or equal it, when nothing was dropped).
        // If this ever failed, pruning would be *inventing* causal history.
        ASSERT_TRUE(dominatesOrEqual(a, p)) << why(a, p, i);
    }
}

TEST(VectorClockProperty, PruneIsDeterministic) {
    std::mt19937_64 rng(kSeed + 10);
    for (int i = 0; i < kCases; ++i) {
        VectorClock a = randomWideClock(rng, 25);
        VectorClock x = a, y = a;
        x.prune(20);
        y.prune(20);
        // Two nodes handed the same clock must drop the same entries, or pruning
        // alone would make replicas diverge.
        ASSERT_EQ(x.serialize(), y.serialize()) << why(x, y, i);
    }
}

// The standout: turn the prose caveat in docs/decisions/tier-4.5.md into an
// executable probe. Pruning is lossy, so in principle it can turn a real
// dominance into a wrong verdict. The claim is that MAX_CLOCK_ENTRIES=20 keeps
// that out of reach, and that the realistic degradation is a *conservative* one
// (CONCURRENT → siblings), never a silently flipped winner.
//
// A false CONCURRENT is acceptable. A flip to B_DOMINATES would be silent data
// loss — a should never lose to something it truly superseded.
TEST(VectorClockProperty, PruningNeverFlipsTheWinnerAtTheProductionBound) {
    std::mt19937_64 rng(kSeed + 11);
    std::uniform_int_distribution<uint64_t> bump(1, 3);
    int dominant_pairs = 0, degraded_to_concurrent = 0;

    for (int i = 0; i < kCases * 5; ++i) {
        // Build b, then a = b with several entries advanced → a genuinely dominates b.
        VectorClock b = randomWideClock(rng, 25);
        VectorClock a = b;
        for (const auto &kv : b.entries()) {
            if (rng() % 3 == 0)
                a.set(kv.first, kv.second.counter + bump(rng), kv.second.updated_ms);
        }
        if (VectorClock::compare(a, b) != Ordering::A_DOMINATES) continue;
        ++dominant_pairs;

        VectorClock pa = a, pb = b;
        pa.prune(20);  // the production bound
        pb.prune(20);

        auto o = VectorClock::compare(pa, pb);
        ASSERT_NE(o, Ordering::B_DOMINATES) << "PRUNING FLIPPED THE WINNER — silent data loss\n"
                                            << why(a, b, i) << "\n  pruned a = " << pa.serialize()
                                            << "\n  pruned b = " << pb.serialize();
        if (o == Ordering::CONCURRENT) ++degraded_to_concurrent;
    }

    EXPECT_GT(dominant_pairs, 0) << "generator never produced a dominant pair — test is vacuous";
    // Recorded, not asserted: degrading to siblings is the documented, acceptable
    // outcome. Surfacing the count keeps the trade-off visible rather than implied.
    RecordProperty("dominant_pairs", dominant_pairs);
    RecordProperty("degraded_to_concurrent", degraded_to_concurrent);
}
