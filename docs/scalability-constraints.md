# Scalability constraints (current, as of Tier 4.5)

What actually limits this system today, from a code-level audit — not from the design docs. This
supersedes the Tier-2 analysis (`docs/decisions/tier-2.md`), which described the pre-gossip world and
is kept only as the record of *why* Tier 4 happened.

**Honest summary:** Tier 4 removed the *architectural* ceilings (membership, transport, gateway
coordination funnel, unbounded clocks). What remains are (a) a few real bottlenecks that would bite
somewhere between 100 and 1000 nodes, (b) some single points of failure that are fine for a demo and
not for production, and (c) **an unproven scaling claim** — the 5→100 benchmark exists but has never
been run, so `bench/scale/RESULTS.md` is empty.

---

## 1. Ceilings that Tier 4 removed (for context)

| Was | Fixed by |
|---|---|
| Membership learned only at JOIN; ring views diverged; no dead-node removal | 4.1 SWIM gossip |
| Writes lost when an owner was down; rarely-read keys never converged | 4.2 hinted handoff (+ Merkle, partly — see §2.1) |
| One socket + one thread per replica call; dead replicas stalled reads 500 ms each | 4.3 connection pool, thread pool, liveness-aware reads |
| Every request coordinated on the first seed node (node1) | 4.4 ring-aware routing |
| Vector clocks grew one entry per coordinator, forever | 4.5 pruning |

---

## 2. Real constraints today

### 2.0 A permanently-dead node keeps its ring slots forever
Since the testing tier, gossip-detected death no longer evicts a node from the ring — that is correct
(a *temporary* failure must not reshuffle key ownership, and evicting made hinted handoff unreachable;
see `docs/decisions/tier-testing.md`). The consequence to accept: a node that is gone **permanently**
still holds its vnodes, so its keys are served by stand-ins whose hints expire after `HINT_TTL_SECONDS`
(3h), leaving those keys under-replicated indefinitely.

Dynamo separates this out as an **administrative removal**, and this system has no such path yet:
`EventType::Leave` exists in the enum but nothing emits it. *Fix:* a `LEAVE`/decommission command that
removes a node from the ring deliberately. Until then, permanent departures need a manual redeploy.

### 2.1 Anti-entropy is not wired across nodes — **the biggest functional gap**
`MerkleTree` is implemented and unit-tested, but `exchange_fn`/`pull_fn` in `src/main.cpp` are
**stubs that return empty**. So nodes never actually compare trees.

**Consequence:** the only convergence mechanisms that truly run are read repair (heals only keys that
are *read*) and hinted handoff (heals only outages the coordinator noticed, within the 3h hint TTL).
A key written during an outage that outlives the hint TTL, and is never read, **can stay divergent
indefinitely**. `antientropy_syncs_total` will sit at 0 — that is expected, not a bug to chase.

*Bounds:* correctness under long//repeated outages. *Fix:* implement the `MERKLE_*` wire protocol.

### 2.2 The `seed` service is a join-time single point of failure
In the Swarm stack, all `kvstore` replicas seed off one `seed` replica. If it's down, **new nodes
cannot join** (existing membership is unaffected — SWIM is peer-to-peer after join). This is a
deliberate trade: seeding off `tasks.kvstore` lets a replica resolve to *itself* and stay invisible
forever (~1/N odds each, near-certain at 100). *Fix:* run 2–3 seed replicas and list them all.

### 2.3 Single-instance components (ingress + state)
- **nginx**: `replicas: 1` — the whole public entry point. Loses ingress if it dies.
- **Postgres**: `replicas: 1`, pinned to the manager by a volume constraint. Gateway metadata
  (audit log, node registry, config versions) is unavailable if it dies. **The KV data path does not
  depend on it** — only gateway metadata does.
- **Prometheus/Grafana**: single, manager-pinned. Observability only.

*Bounds:* availability, not data-path scale.

### 2.4 The load generator and gateway tier saturate before the cluster
k6 and 2 gateway replicas will very likely plateau before 100 nodes do. If the scaling curve flattens,
**check gateway/nginx/k6 CPU before concluding the cluster stopped scaling** — this is the single
easiest way to misread the benchmark.

### 2.5 Server is connection-bound, not event-driven
`ThreadPool` (4.3) dedicates a worker to a connection for its lifetime; default `WORKER_THREADS=64`.
Fine while client-side pooling caps inbound connections (~`max_per_peer` × peers + gateways ≈ a
dozen). A flood of *unpooled* clients could exhaust workers and queue new connections. True C10K needs
epoll — explicitly out of scope (`docs/decisions/tier-4.3.md`). *Lever:* raise `WORKER_THREADS`.

### 2.6 Not everything is pooled
Only the coordinator's **replica fan-out** uses the connection pool. The **forward-to-primary** path
(`Node::handlePut`/`handleDelete`) and the **gossip send path** still open a one-shot socket per call.
Gossip is one probe/second/node, so it's minor — but it's per-node socket churn that grows with N.

### 2.7 Gossip's per-node cost is O(1), but total traffic is O(N)
Each node probes one random peer per second plus K=3 indirect probes on failure — constant per node.
Cluster-wide that's O(N) messages/second. At 1000 nodes ≈ 1000 probes/sec cluster-wide, which is fine;
the property to *verify in the benchmark* is that convergence time grows ~O(log N), not O(N). **If
convergence grows linearly, the extrapolation past 100 does not hold.**

### 2.8 Vector-clock pruning is lossy in principle
Bounded to `MAX_CLOCK_ENTRIES=20`. The realistic degradation is a false CONCURRENT (siblings —
conservative). A false *dominance* (silent overwrite) is possible only if a clock sheds **every** entry
carrying its dominance, which 20 retained entries puts out of practical reach. Dynamo makes the same
trade. **Lowering `MAX_CLOCK_ENTRIES` aggressively moves this from theoretical toward real.**
Full reasoning: `docs/decisions/tier-4.5.md`.

### 2.9 Gateway ring staleness (5 s)
`RingPoller` refreshes every `cluster.ring-poll-interval-ms` (5000). For up to 5 s after a membership
change the gateway may route to a departed node. **This costs a retry/hop, never correctness** — the
receiving node always coordinates against its *own* ring, and `ClusterClient` fails over. Routing is a
pure optimization over a correct base.

### 2.10 vnode count must match — and mismatches are silent
The gateway's `cluster.virtual-nodes` (128) must equal the nodes' `VNODES` (128). If they diverge, the
client ring won't line up and **every** request silently degrades to the forward-hop fallback: slower,
still correct, and easy to miss. Both default to 128 and neither asserts. *Fix:* expose vnode count on
`RING` and assert at startup.

### 2.11 `forEach` blocks the in-memory engine (not RocksDB)
Anti-entropy scans storage every 5 min. On **RocksDB (production default) there is no stall** — it
iterates via a RocksDB iterator with no engine-wide lock. On `InMemoryStorageEngine`
(`STORAGE_ENGINE=memory`) the scan holds the mutex and blocks reads/writes. Only affects memory-engine
runs. (Predicted in `docs/decisions/tier-0.md`.)

### 2.12 Node storage is ephemeral in the container
Compose declares no volume for node data; RocksDB lives in the container's writable layer. `docker
compose down` (or any `rm`) **wipes node data**. That's *convenient* right now (the 4.4/4.5 breaking
changes require a fresh ring anyway) but means the cluster is not durable across a recreate.

---

## 3. Fixed during the audit and the testing tier

### 3.-1 Three more bugs, found by the in-process cluster harness on its first run
All were live in `main` and all had shipped through a green suite
(`docs/decisions/tier-testing.md`):

- **Hinted handoff was unreachable — Tier 4.2 had never stored a single hint.** A node is `isAlive`
  while `Alive` *or* `Suspect`, and the moment it turned `Dead` gossip removed it from the ring, so it
  was never a "dead owner" the coordinator could hint for. The live `hints_stored_total` of 0 was not
  "no outage yet" — it was impossible. Fixed by keeping dead nodes in the ring (§2.0).
- **A same-incarnation relay cleared suspicion, forever.** Combined with gossip re-enqueueing anything
  that changed state, a stale event flipped Suspect→Alive, got re-disseminated, and came back — so a
  dead node could never be declared dead. Cluster tests took 30s before the fix, 0.15s after.
- **A fabricated ack** let one physical replica satisfy two acks when no stand-in existed, so `W=3`
  could "succeed" against two replicas.

### 3.0 A restarted node could never rejoin — **the most serious bug found**
A node that restarts begins again at **incarnation 0**, while the cluster still holds it `Dead` at
incarnation ≥ 0. SWIM's revive rule demanded a *strictly higher* incarnation, so `0 <= 0` → the join
was rejected and the node stayed `Dead` **forever**: alive, healthy, and receiving zero traffic.

Observed live — after a kill/restart the ring reported only `node1` + `node3`, and node2's own log
showed the tell-tale asymmetry: it had happily added node1 and node3 to *its* ring, while they had
permanently written it off.

**Why it mattered more than lost capacity:** hinted handoff fires on the `Dead → Alive` transition. A
node that can never transition back means **hints are stored and never delivered** — they just expire
after the 3 h TTL. Tier 4.2's headline recovery feature was silently inert in exactly the scenario it
exists for.

**Fix:** a `Join` is the node speaking for *itself* through the join handshake — authoritative proof of
liveness, accepted regardless of incarnation. The incarnation guard still applies to relayed `Alive`
gossip, which is where stale-resurrection actually matters (`StaleAliveGossipStillCannotReviveDead`
covers that). Verified end-to-end: kill → evict → restart → **rejoin**.

**Why no test caught it:** `FreshJoinRevivesDead` used an artificially higher incarnation (5) — it
encoded the bug's assumption instead of a real restart. `RestartedNodeRejoinsAtSameIncarnation` now
pins the real scenario. A reminder that a passing suite only proves what it actually models.

### Hot paths that grew with cluster size
Both would have suppressed the very linear-scaling curve the 4.5 benchmark is meant to demonstrate:

1. **`RingRouter.ownersFor` recomputed the distinct-physical count on every request**, iterating all
   `vnodes × N` ring entries — 12,800 at 100 nodes, 128,000 at 1000, **per gateway request**. Now
   precomputed once per ring rebuild and published with the snapshot.
2. **`Coordinator::writeQuorum` copied the entire membership on every write** (`getAllPhysicalNodes()`
   under a lock) to serve a dead-owner branch that almost never runs. Now fetched lazily, at most once,
   and only when an owner is actually dead.

---

## 4. What would break first, in order

Best engineering guess for where this system fails as it grows — to be replaced by measurements once
the benchmark runs:

1. **The gateway/k6 tier** saturates (§2.4) — probably well before the cluster does.
2. **Convergence time** if gossip doesn't hold O(log N) (§2.7) — the property that decides whether
   "scales to thousands" is credible at all.
3. **Correctness under long outages**, because anti-entropy isn't wired (§2.1) — silent divergence.
4. **Worker exhaustion** (§2.5) only under many unpooled clients.

## 5. The claim this project has *not* yet earned

The Tier 4 thesis is "scales to thousands of nodes." That is **currently unproven**:
`bench/scale/RESULTS.md` is empty because the multi-host run hasn't happened. The harness argues by
*shape* (throughput rises, p99 bounded, convergence ~O(log N) from 5→100) rather than by reaching the
target. Until those numbers exist, the honest statement is: *"the known architectural blockers to
scaling have been removed and the design extrapolates — unmeasured."*
