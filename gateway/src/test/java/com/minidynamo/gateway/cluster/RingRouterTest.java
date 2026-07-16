package com.minidynamo.gateway.cluster;

import static org.assertj.core.api.Assertions.assertThat;

import java.util.List;
import org.junit.jupiter.api.Test;

/**
 * Verifies the Java ring reproduces the C++ ring exactly. The expected owner
 * orderings were produced by the real {@code Router::findOwners} in
 * {@code src/router.cpp} for the same 3-node set at the default 128 vnodes —
 * so passing this test proves the gateway routes to the same primary the cluster
 * would pick (host/port are irrelevant to placement; only the node id feeds the
 * vnode hash).
 */
class RingRouterTest {

    private RingRouter ring3() {
        RingRouter r = new RingRouter(128);
        r.rebuild(List.of(
                new RingNode("node1", "node1", 5001),
                new RingNode("node2", "node2", 5002),
                new RingNode("node3", "node3", 5003)));
        return r;
    }

    private static List<String> ids(List<RingNode> owners) {
        return owners.stream().map(RingNode::id).toList();
    }

    @Test
    void ownersMatchCppRouter() {
        RingRouter r = ring3();
        assertThat(ids(r.ownersFor("key1", 3))).containsExactly("node2", "node3", "node1");
        assertThat(ids(r.ownersFor("user:42", 3))).containsExactly("node2", "node3", "node1");
        assertThat(ids(r.ownersFor("hello", 3))).containsExactly("node1", "node2", "node3");
        assertThat(ids(r.ownersFor("the quick brown fox", 3)))
                .containsExactly("node2", "node3", "node1");
        assertThat(ids(r.ownersFor("apple", 3))).containsExactly("node2", "node3", "node1");
        assertThat(ids(r.ownersFor("banana", 3))).containsExactly("node1", "node2", "node3");
        assertThat(ids(r.ownersFor("cart-99", 3))).containsExactly("node3", "node2", "node1");
        assertThat(ids(r.ownersFor("session-xyz", 3))).containsExactly("node1", "node2", "node3");
    }

    @Test
    void primaryIsFirstOwner() {
        RingRouter r = ring3();
        assertThat(r.primaryFor("key1").id()).isEqualTo("node2");
        assertThat(r.primaryFor("hello").id()).isEqualTo("node1");
        assertThat(r.primaryFor("cart-99").id()).isEqualTo("node3");
    }

    @Test
    void ownersAreDistinctAndCappedAtNodeCount() {
        RingRouter r = ring3();
        // Asking for more owners than nodes returns each physical node once.
        List<RingNode> owners = r.ownersFor("key1", 10);
        assertThat(ids(owners)).containsExactlyInAnyOrder("node1", "node2", "node3");
    }

    @Test
    void emptyRingReturnsNoOwners() {
        RingRouter r = new RingRouter(128);  // never rebuilt
        assertThat(r.ownersFor("key1", 3)).isEmpty();
        assertThat(r.primaryFor("key1")).isNull();
    }

    @Test
    void rebuildReflectsNewNodeSet() {
        RingRouter r = ring3();
        assertThat(r.physicalNodeCount()).isEqualTo(3);
        r.rebuild(List.of(new RingNode("node1", "node1", 5001)));
        assertThat(r.physicalNodeCount()).isEqualTo(1);
        assertThat(r.primaryFor("anything").id()).isEqualTo("node1");
    }
}
