# Tier 4.2: Hinted Handoff + Merkle-Tree Anti-Entropy

## What was built

1. **Hinted Handoff** — sloppy quorum writes that survive single-node failures:
   - `HintStore` (in-memory, TTL-expiring, per-target-capped hint storage)
   - `HandoffThread` (listens for gossip recovery events, delivers hints on wake)
   - Coordinator's `writeQuorum` modified: when a target is detected dead via SWIM, the coordinator picks the next alive node as a stand-in and stores a hint for later delivery

2. **Merkle-Tree Anti-Entropy** — background detection and repair of silent divergence:
   - `MerkleTree` (binary hash tree, depth 15 = 32K leaves, XOR-based, O(log N) diff)
   - `AntiEntropyThread` (periodic: build local tree, exchange with random peer, drill into divergent ranges, pull/push keys by vector clock comparison)

3. **Metrics** — `incHintStored`, `incHintDelivered`, `incAntiEntropySync`, `incAntiEntropyKeysRepaired` added to the Metrics interface, InMemoryMetrics, and PrometheusMetrics.

4. **Wiring** — main.cpp instantiates HintStore, HandoffThread, AntiEntropyThread; registers gossip callback for recovery events; injects liveness check + hint store into the coordinator.

## Key design choices

### Sloppy quorum in the coordinator (not the node handler)
**Chose:** Coordinator detects dead targets and redirects writes at the quorum-assembly stage.
**Rejected:** Node-level forwarding (the node handler detects failure and re-routes). This couples protocol parsing with liveness logic and makes the forwarding path opaque to quorum counting.

### In-memory HintStore (not RocksDB column family)
**Chose:** std::unordered_map with mutex, TTL expiry, per-target cap.
**Rejected:** RocksDB column family for crash-durable hints. Hints live seconds-to-minutes (the recovered node comes back quickly); a process crash during hint delivery is exceedingly unlikely to lose hints that matter. The in-memory path is simpler, faster, and avoids column-family migration complexity. If crash durability becomes needed, the HintStore interface is already abstract enough to swap in.

### XOR-based Merkle tree (not SHA-based)
**Chose:** Each leaf is an XOR of its key-value hashes; internal nodes are XOR of children.
**Rejected:** SHA-256 per node (cryptographically secure). XOR is collision-prone in adversarial settings, but this is an internal protocol between trusted replicas, not a security boundary. XOR is ~100x faster to compute and lets the tree be rebuilt every 5 minutes without noticeable CPU cost.

### Pull-based anti-entropy (initiator pulls from peer)
**Chose:** Initiator builds local tree, requests peer's tree, diffs, then pulls missing keys from the peer and pushes keys where it's ahead.
**Rejected:** Push-only (initiator pushes everything it has that's divergent). Pull-based handles the common case better: a recovered node that missed writes during its outage pulls the newer data it's missing rather than relying on every other node to push to it.

### Anti-entropy exchange functions are stubs in main.cpp
**Chose:** exchange_fn and pull_fn are placeholder lambdas. The tree comparison works correctly in-process (unit-tested); the wire protocol for exchanging serialized trees between nodes is deferred to integration wiring.
**Rejected:** Building a full MERKLE_EXCHANGE wire protocol now. The hinted-handoff path already handles the acute problem (writes during outage); anti-entropy catches long-tail divergence and can tolerate a stub exchange until Tier 4.3's connection pooling makes the multi-round-trip tree exchange cheap.

## Where it could break

1. **Stand-in selection is linear scan**: `writeQuorum` iterates `getAllPhysicalNodes()` to find a stand-in. At 1000 nodes this is O(N) per dead-target-per-write. If multiple nodes are dead simultaneously and writes are hot, this becomes a bottleneck. Fix: maintain a pre-shuffled "alive successor" list updated on each gossip tick.

2. **Hint TTL vs. long outage**: Default TTL is 3 hours. A node down for >3 hours loses all its hints — those writes are only recoverable via anti-entropy (which is currently stubbed for cross-node exchange). Until the wire protocol is wired, a >3h outage can leave silent divergence unrepaired.
