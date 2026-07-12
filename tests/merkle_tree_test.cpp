#include "gtest/gtest.h"
#include "antientropy/merkle_tree.h"

TEST(MerkleTreeTest, EmptyTreeHasZeroRoot) {
    MerkleTree tree;
    tree.build();
    EXPECT_EQ(tree.rootHash(), 0u);
}

TEST(MerkleTreeTest, SingleInsert) {
    MerkleTree tree;
    tree.insert(0x8000000000000000ULL, 42);
    tree.build();
    EXPECT_NE(tree.rootHash(), 0u);
}

TEST(MerkleTreeTest, IdenticalTreesNoDiff) {
    MerkleTree a, b;
    a.insert(100, 200);
    a.insert(300, 400);
    a.build();

    b.insert(100, 200);
    b.insert(300, 400);
    b.build();

    auto diffs = MerkleTree::diff(a, b);
    EXPECT_TRUE(diffs.empty());
}

TEST(MerkleTreeTest, DifferentTreesProduceDiff) {
    MerkleTree a, b;
    a.insert(100, 200);
    a.build();

    b.insert(100, 999);  // same key, different value hash
    b.build();

    auto diffs = MerkleTree::diff(a, b);
    EXPECT_FALSE(diffs.empty());
}

TEST(MerkleTreeTest, DisjointKeysProduceDiff) {
    MerkleTree a, b;
    // Keys that hash to very different leaf positions.
    a.insert(0x0000000000000001ULL, 1);
    a.build();

    b.insert(0xFFFFFFFF00000000ULL, 2);
    b.build();

    auto diffs = MerkleTree::diff(a, b);
    // Both trees are non-empty and diverge — expect diffs.
    EXPECT_FALSE(diffs.empty());
}

TEST(MerkleTreeTest, DiffRangeCoversCorrectLeaf) {
    MerkleTree a, b;
    // Insert into a specific high-order leaf position (top bits determine leaf).
    uint64_t key_hash = 0x8000000000000000ULL;  // leaf index = 2^14 = 16384
    a.insert(key_hash, 42);
    a.build();

    b.build();  // empty

    auto diffs = MerkleTree::diff(a, b);
    ASSERT_FALSE(diffs.empty());

    // The diff should cover leaf 16384 (half the range).
    bool found = false;
    for (const auto &dr : diffs) {
        if (dr.range_start <= key_hash && key_hash <= dr.range_end) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(MerkleTreeTest, ClearResetsTree) {
    MerkleTree tree;
    tree.insert(100, 200);
    tree.build();
    EXPECT_NE(tree.rootHash(), 0u);

    tree.clear();
    tree.build();
    EXPECT_EQ(tree.rootHash(), 0u);
}

TEST(MerkleTreeTest, XORPropertyAllowsRemoval) {
    // Inserting the same (key_hash, value_hash) twice XORs to zero — effectively
    // removing the contribution. This is the property that lets anti-entropy
    // detect when a key is deleted on one side.
    MerkleTree tree;
    tree.insert(100, 200);
    tree.insert(100, 200);  // XOR cancels
    tree.build();
    EXPECT_EQ(tree.rootHash(), 0u);
}

TEST(MerkleTreeTest, DifferentDepthsNoDiff) {
    MerkleTree a(10);
    MerkleTree b(15);
    a.insert(100, 200);
    b.insert(100, 200);
    a.build();
    b.build();

    // Different depths → diff returns empty (cannot compare).
    auto diffs = MerkleTree::diff(a, b);
    EXPECT_TRUE(diffs.empty());
}
