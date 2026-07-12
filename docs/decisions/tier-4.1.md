# Tier 4.1 — Gossip/SWIM Membership + Failure Detection (decisions log)

## What was built

A SWIM-style gossip protocol (Scalable Weakly-consistent Infection-style
Membership) that replaces the single-bootstrap JOIN model. Every node now
converges on an identical ring view via epidemic dissemination, and dead nodes
are automatically detected and removed.

- **`src/gossip/swim.h` / `.cpp`** — The SWIM state machine: member list,
  incarnation numbers, Alive→Suspect→Dead transitions, self-refutation,
  dissemination queue (piggybacked events drained over 3·log₂(N) rounds).
- **`src/gossip/gossip_thread.h` / `.cpp`** — The protocol driver: a dedicated
  background thread that wakes every `protocol_period` (default 1 s), pings one
  random peer, escalates to K=3 indirect probes on failure, and expires suspects
  after `suspicion_mult × protocol_period` (default 5 s). Also handles incoming
  SWIM_PING/PING_REQ/ACK/JOIN messages (called from Node::handleRequest).
- **`src/gossip/member_event.h` / `.cpp`** — Membership event types (Join,
  Alive, Suspect, Dead, Leave) with compact text serialization for piggybacking.
- **`src/router.h` / `.cpp`** — `std::mutex` → `std::shared_mutex` (reads no
  longer block each other). Virtual nodes per physical node increased from 3 →
  128 for adequate distribution at thousands of nodes.
- **`src/main.cpp`** — `SEED_NODES` env var replaces `BOOTSTRAP_IP`/`PORT`.
  Backward-compatible: if only `BOOTSTRAP_IP` is set, it's treated as a single
  seed. Gossip thread owns the Router membership lifecycle.
- **`src/node.h` / `.cpp`** — SWIM message dispatch routed to gossip thread.
- **`src/net/tcp_server.cpp`** — Accept backlog 10 → 1024.
- **`docker-compose.yml`** — `SEED_NODES: node1:5001` on node2/node3.

## Key design choices

1. **SWIM with suspicion, not basic heartbeat gossip.** SWIM gives O(1)
   per-member per-period overhead (bounded regardless of cluster size) and
   strong completeness. A naïve all-to-all heartbeat would be O(N²) at 1000
   nodes. *Rejected:* basic random-pair gossip (Cassandra-style) — less
   formally analyzed failure detection; we wanted the paper's guarantees.

2. **Piggyback dissemination on protocol messages.** Membership events ride
   existing PING/ACK traffic rather than dedicated gossip rounds — zero extra
   network cost. Each event is sent on 3·log₂(N) messages, giving O(log N)
   convergence. *Rejected:* dedicated dissemination channel — doubles traffic
   for no benefit when piggyback achieves the same properties.

3. **Seed list (2-3 addresses), not single bootstrap.** Removes the join SPOF.
   Seeds are normal nodes after startup; any seed can admit a joiner and the
   SWIM dissemination propagates the join to all. *Rejected:* keeping single
   `BOOTSTRAP_IP` — ceiling on reliability and join throughput.

4. **Extend existing wire format (not protobuf).** New message types
   (`SWIM_PING|...`, `SWIM_JOIN|...`) in the same length-prefixed pipe-delimited
   format. Keeps the protocol human-readable and the build simple. *Rejected:*
   protobuf/gRPC — adds a code-gen step and dependency disproportionate for this
   educational project. Evaluated for Tier 4.3.

5. **128 virtual nodes per physical node (was 3).** At 1000 nodes, 128K ring
   positions give adequate key-space uniformity (Cassandra/Riak standard).
   3 vnodes at scale would produce 5× load imbalances. *Rejected:* 256 —
   overkill; memory for 256K ring entries starts to matter.

6. **`shared_mutex` for the ring (was `mutex`).** The read path
   (`findOwners`, every request) now takes a shared lock; the write path
   (`add/removePhysicalNode`, rare membership changes) takes an exclusive lock.
   Under normal operation the ring is read-only and the shared lock is
   uncontended. *Rejected:* lock-free copy-on-write — more complex and the write
   path is so rare that shared_mutex gives the same practical benefit.

## Where this could break

- **Gossip over slow/lossy links.** The 200 ms ping timeout is tuned for a LAN
  (Docker bridge or same-AZ). Cross-region deployments would need a higher
  timeout and possibly a longer protocol period, else healthy nodes get
  falsely suspected.
- **Incarnation number overflow.** `uint64_t` — practically impossible to
  exhaust, but theoretically a pathologically flapping node could burn through
  incarnations.
- **Clock skew in suspicion timeout.** `steady_clock` is monotonic so this is
  safe per-node, but if protocol period drifts across nodes (e.g., one node
  under severe CPU starvation), false suspicions could cascade.
- **No protocol versioning.** If the SWIM wire format ever changes, there's no
  negotiation — a mixed-version cluster would misparse messages. Acceptable for
  now; a version field in the SWIM message is the simple fix.
