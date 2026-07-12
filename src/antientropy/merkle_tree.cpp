#include "merkle_tree.h"

#include <algorithm>
#include <cstring>
#include <limits>

MerkleTree::MerkleTree(int depth) : depth_(depth) {
    leaf_start_ = static_cast<size_t>(1) << depth;
    size_t total = leaf_start_ * 2;  // 2^(depth+1) nodes in level-order
    nodes_.resize(total, 0);
}

void MerkleTree::insert(uint64_t key_hash, uint64_t value_hash) {
    // Map key_hash to a leaf using the top `depth_` bits.
    size_t leaf_idx = key_hash >> (64 - depth_);
    nodes_[leaf_start_ + leaf_idx] ^= value_hash;
}

void MerkleTree::build() {
    // Bottom-up: each internal node is XOR of its children.
    for (size_t i = leaf_start_ - 1; i >= 1; --i) {
        nodes_[i] = nodes_[2 * i] ^ nodes_[2 * i + 1];
    }
}

uint64_t MerkleTree::rootHash() const {
    return nodes_.size() > 1 ? nodes_[1] : 0;
}

uint64_t MerkleTree::nodeHash(size_t index) const {
    if (index >= nodes_.size()) return 0;
    return nodes_[index];
}

void MerkleTree::clear() {
    std::fill(nodes_.begin(), nodes_.end(), 0);
}

std::vector<MerkleTree::DiffRange> MerkleTree::diff(const MerkleTree &a, const MerkleTree &b) {
    std::vector<DiffRange> result;
    if (a.depth_ != b.depth_) return result;
    if (a.rootHash() == b.rootHash()) return result;

    // BFS top-down: only descend into subtrees that differ.
    std::vector<size_t> queue;
    queue.push_back(1);  // root

    size_t leaf_start = a.leaf_start_;
    int depth = a.depth_;

    while (!queue.empty()) {
        std::vector<size_t> next;
        for (size_t idx : queue) {
            if (a.nodes_[idx] == b.nodes_[idx]) continue;
            if (idx >= leaf_start) {
                // This is a leaf — record the diff range.
                size_t leaf_idx = idx - leaf_start;
                uint64_t range_bits = static_cast<uint64_t>(leaf_idx) << (64 - depth);
                uint64_t range_size = static_cast<uint64_t>(1) << (64 - depth);
                DiffRange dr;
                dr.leaf_index = leaf_idx;
                dr.range_start = range_bits;
                dr.range_end = (range_size == 0) ? std::numeric_limits<uint64_t>::max()
                                                 : range_bits + range_size - 1;
                result.push_back(dr);
            } else {
                next.push_back(2 * idx);
                next.push_back(2 * idx + 1);
            }
        }
        queue = std::move(next);
    }
    return result;
}
