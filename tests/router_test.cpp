#include "router.h"

#include <gtest/gtest.h>

#include <set>

namespace {
NodeInfo n(const std::string &id) { return NodeInfo(id, id, 5000); }
}  // namespace

TEST(Router, ReturnsNDistinctPhysicalOwners) {
    Router r;
    r.addPhysicalNode(n("node1"));
    r.addPhysicalNode(n("node2"));
    r.addPhysicalNode(n("node3"));
    r.addPhysicalNode(n("node4"));

    auto owners = r.findOwners("some-key", 3);
    ASSERT_EQ(owners.size(), 3u);
    std::set<std::string> ids;
    for (auto &o : owners) ids.insert(o.node_id);
    EXPECT_EQ(ids.size(), 3u) << "owners must be distinct physical nodes";
}

TEST(Router, CapsAtClusterSizeWhenReplicasExceedNodes) {
    Router r;
    r.addPhysicalNode(n("node1"));
    r.addPhysicalNode(n("node2"));

    auto owners = r.findOwners("k", 5);
    EXPECT_EQ(owners.size(), 2u);
}

TEST(Router, EmptyRingReturnsNothing) {
    Router r;
    EXPECT_TRUE(r.findOwners("k", 3).empty());
}

TEST(Router, PlacementIsStableForSameKey) {
    Router r;
    r.addPhysicalNode(n("node1"));
    r.addPhysicalNode(n("node2"));
    r.addPhysicalNode(n("node3"));
    auto ids = [](const std::vector<NodeInfo> &v) {
        std::vector<std::string> out;
        for (auto &x : v) out.push_back(x.node_id);
        return out;
    };
    EXPECT_EQ(ids(r.findOwners("k", 2)), ids(r.findOwners("k", 2)));
}
