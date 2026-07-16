package com.minidynamo.gateway.cluster;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.NavigableMap;
import java.util.Set;
import java.util.TreeMap;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

/**
 * A client-side copy of the cluster's consistent-hash ring, so the gateway can
 * pick a key's owners without a round trip. It mirrors {@code Router} in
 * {@code src/router.cpp}: each physical node is placed at {@code virtualNodes}
 * positions {@code hash64(nodeId + "#vn" + i)}, and {@link #ownersFor} walks the
 * ring clockwise collecting distinct physical owners — identical semantics to
 * {@code Router::findOwners}.
 *
 * <p>The ring is published as an immutable snapshot: {@link #rebuild} builds a
 * fresh map and swaps the {@code volatile} reference, so concurrent readers
 * always see a consistent ring with no locking. If the ring is empty (before the
 * first poll) {@link #ownersFor} returns empty and the caller falls back to its
 * static node list — routing is a best-effort optimization, never a correctness
 * dependency.
 */
@Component
public class RingRouter {

    private final int virtualNodes;

    // TreeMap keyed by unsigned-compared Long, matching the C++ std::map<uint64_t>
    // ordering. Volatile: rebuilt-and-swapped wholesale, never mutated in place.
    private volatile NavigableMap<Long, RingNode> ring = emptyRing();

    // virtualNodes MUST equal the C++ node default (128) or ring positions won't
    // line up and routing degrades to the forward-hop fallback.
    public RingRouter(@Value("${cluster.virtual-nodes:128}") int virtualNodes) {
        this.virtualNodes = virtualNodes;
    }

    private static NavigableMap<Long, RingNode> emptyRing() {
        return new TreeMap<>(Long::compareUnsigned);
    }

    /** Replace the ring with vnodes for the given physical nodes. */
    public void rebuild(List<RingNode> nodes) {
        NavigableMap<Long, RingNode> next = emptyRing();
        for (RingNode node : nodes) {
            for (int i = 0; i < virtualNodes; i++) {
                next.put(HashUtil.hash64(node.id() + "#vn" + i), node);
            }
        }
        ring = next;  // atomic publish of the new snapshot
    }

    /** The N distinct physical owners of {@code key}, primary first; empty if the ring is unbuilt. */
    public List<RingNode> ownersFor(String key, int n) {
        if (n <= 0) n = 1;
        NavigableMap<Long, RingNode> snap = ring;  // read the snapshot once
        if (snap.isEmpty()) return List.of();

        long h = HashUtil.hash64(key);
        // Start at the first vnode >= h (unsigned), wrapping to the first entry —
        // the clockwise walk of Router::findOwners.
        Map.Entry<Long, RingNode> start = snap.ceilingEntry(h);
        Long cursor = (start != null) ? start.getKey() : snap.firstKey();

        Set<RingNode> owners = new LinkedHashSet<>();  // preserves ring order, dedups physicals
        int distinctPhysicalCap = countPhysicals(snap);
        while (owners.size() < n) {
            owners.add(snap.get(cursor));
            if (owners.size() == distinctPhysicalCap) break;  // seen every physical node
            Long nextKey = snap.higherKey(cursor);
            cursor = (nextKey != null) ? nextKey : snap.firstKey();  // wrap
        }
        return new ArrayList<>(owners);
    }

    /** The primary owner of {@code key}, or null if the ring is unbuilt. */
    public RingNode primaryFor(String key) {
        List<RingNode> owners = ownersFor(key, 1);
        return owners.isEmpty() ? null : owners.get(0);
    }

    /** Number of physical nodes currently on the ring (for tests/observability). */
    public int physicalNodeCount() {
        return countPhysicals(ring);
    }

    private static int countPhysicals(NavigableMap<Long, RingNode> snap) {
        Set<String> ids = new LinkedHashSet<>();
        for (RingNode n : snap.values()) ids.add(n.id());
        return ids.size();
    }
}
