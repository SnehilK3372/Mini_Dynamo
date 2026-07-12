#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A binary Merkle hash tree over a key range [0, 2^64). Used for anti-entropy:
// two replicas compare trees top-down to efficiently identify divergent
// sub-ranges, then sync only those keys. Depth 15 → 32768 leaf buckets.
class MerkleTree {
   public:
    static constexpr int kDefaultDepth = 15;
    static constexpr size_t kLeafCount = 1u << kDefaultDepth;  // 32768

    explicit MerkleTree(int depth = kDefaultDepth);

    // Insert a key's contribution (hash of key + serialized value).
    // The key is mapped to a leaf by its high bits; the leaf hash accumulates
    // via XOR (commutative + associative → order-independent).
    void insert(uint64_t key_hash, uint64_t value_hash);

    // Rebuild internal nodes from leaves up to root.
    void build();

    // Root hash (0 if tree is empty or not yet built).
    uint64_t rootHash() const;

    // Get hash at a specific node index (level-order, root=1).
    uint64_t nodeHash(size_t index) const;

    // Number of internal + leaf nodes (2^(depth+1) - 1 total, stored as 2^(depth+1)).
    size_t nodeCount() const { return nodes_.size(); }

    // Identify leaf indices where two trees differ. Returns pairs of
    // (leaf_index, range_start_hash) for differing leaves.
    struct DiffRange {
        size_t leaf_index;
        uint64_t range_start;
        uint64_t range_end;
    };
    static std::vector<DiffRange> diff(const MerkleTree &a, const MerkleTree &b);

    // Reset all nodes to zero.
    void clear();

    int depth() const { return depth_; }

   private:
    int depth_;
    size_t leaf_start_;  // index of first leaf in level-order (2^depth)
    // Level-order binary tree. Index 0 is unused; root is index 1.
    // Leaves are at indices [leaf_start_, leaf_start_ + leaf_count).
    std::vector<uint64_t> nodes_;
};
