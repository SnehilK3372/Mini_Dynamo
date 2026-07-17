# Mini Dynamo — Roadmap (Tier 4 onward)

Status of the horizontal-scalability arc and the plan for the remaining work. Each sub-tier is
independently mergeable; existing `scripts/e2e.sh` must stay green after each.

## Status

| Sub-tier | Scope | State |
|----------|-------|-------|
| 4.1 | Gossip/SWIM membership + failure detection | ✅ merged |
| 4.2 | Hinted handoff + Merkle anti-entropy | ✅ merged |
| 4.3 | Connection pooling + thread pool + liveness-aware reads | ✅ merged (PR #6) |
| 4.4 | Horizontal gateways + ring-aware routing | ✅ merged (PR #7) |
| 4.5 | Multi-host deploy + vector-clock pruning + scale benchmark | 🚧 code complete; benchmark pending a multi-host run |

Per-tier design records live in `docs/decisions/tier-4.<n>.md`; the approved
implementation plans in `docs/plans/tier-4.<n>.md`.

> **Deploying 4.4 + 4.5 together is a hard cutover.** 4.4 changed the ring hash and
> 4.5 changed the vector-clock wire format. Both reshuffle/reinterpret stored data,
> and a cluster must run a single build. Bring the stack down with
> `docker compose down -v` (or redeploy the Swarm stack) so nodes start on a fresh
> ring — a mixed-build cluster splits key placement.

---

## Tier 4.5 — Multi-Host Deploy + Vector-Clock Pruning + Scale Benchmark

> **Status: code complete, benchmark pending.** Pruning and the Swarm stack are built and unit-tested;
> the 5→100 curve needs an actual multi-host run to fill in `bench/scale/RESULTS.md`. Full design record
> in `docs/decisions/tier-4.5.md`, approved plan in `docs/plans/tier-4.5.md`. The rest of this section is
> kept as the tier's rationale and acceptance criteria.

**What:** Deploy across multiple hosts (Docker Swarm), bound vector-clock growth, and produce a
scaling-curve benchmark that demonstrates the architecture scales.

**Why last:** validates that all prior machinery (gossip, hinted handoff, anti-entropy, pooling,
ring-aware gateways) works across real network boundaries, and prevents unbounded clock growth at
thousands of coordinators.

### Design

**Multi-host (Docker Swarm):**
- Overlay network; services auto-distribute across swarm workers.
- `SEED_NODES=tasks.kvstore:5001` uses Swarm DNS for discovery.
- Same compose-based syntax via a `deploy/swarm/docker-stack.yml`.

**Vector-clock pruning:**
- Each clock entry carries a `last_updated` timestamp.
- When a clock exceeds `MAX_CLOCK_ENTRIES` (default 20), prune the oldest entries by timestamp.
- Worst case of over-pruning is a *false concurrent* (siblings surfaced), **never silent data loss** —
  a pruned clock may compare CONCURRENT where it would have dominated, but never falsely dominates.
- Applied in the coordinator after bumping the clock, before storing.

**Scaling benchmark:**
- Clusters of 5, 10, 20, 50, 100 nodes across 3–5 Swarm hosts.
- k6 uniform workload per size (e.g. 1000 concurrent clients, 70/30 read/write) through nginx.
- Measure: throughput (ops/s), latency (p50/p95/p99), gossip convergence time, failure-detection time,
  hint-delivery time.
- Thesis: near-linear throughput 5→100 with bounded latency and O(1) per-node gossip overhead implies
  1000+ by extrapolation.

### New files
- `deploy/swarm/docker-stack.yml`, `deploy/swarm/init-swarm.sh`, `deploy/swarm/deploy.sh`
- `bench/scale/scale_test.sh`, `bench/scale/k6_uniform.js`, `bench/scale/RESULTS.md`
- `tests/multi_host_smoke.sh`

### Modified files
- `src/vector_clock.h` / `.cpp` — `last_updated` timestamps, `prune(int max_entries)`
- `src/coordinator.cpp` — call `prune()` after bumping the clock
- `docker-compose.yml` — a `deploy:` section for Swarm compatibility
- `deploy/prometheus/prometheus.yml` — dynamic target discovery (DNS SD / file_sd)
- `tests/vector_clock_test.cpp` — pruning tests

### Done when
- Cluster operates correctly across multiple Swarm hosts (overlay network); gossip converges across
  host boundaries.
- Vector clocks bounded to `MAX_CLOCK_ENTRIES` (verified after many distinct coordinators write a key),
  and pruning degrades conservatively — a pruned clock surfaces siblings rather than silently picking a
  winner. (Not an absolute guarantee: a false dominance is possible in principle, which is why the bound
  is generous. See the honest write-up in `docs/decisions/tier-4.5.md`.)
- Scaling curve shows near-linear throughput 5→100 nodes, bounded p99, gossip convergence within
  `O(log N)·T`, failure detection within the suspicion timeout regardless of size.
- Results written up in `bench/scale/RESULTS.md`.

---

## Candidate follow-ups (beyond Tier 4, unscheduled)

Small, valuable items surfaced while building Tier 4; not committed to a tier yet:

- **Pool the forward-to-primary and gossip send paths** with the Tier-4.3 `ConnectionPool` (today only
  the coordinator's replica fan-out is pooled).
- **Finish the anti-entropy wire protocol** — the Merkle *tree logic* is unit-tested, but cross-node
  tree exchange is still stubbed in `main.cpp` (documented in `docs/decisions/tier-4.2.md`), so silent
  divergence isn't yet repaired end-to-end.
- **Startup assert on vnode-count agreement** between the gateway (`cluster.virtual-nodes`) and the
  nodes (`VNODES`) — a silent mismatch degrades routing to the forward-hop fallback.
- **Async I/O (epoll) server** if the node ever needs true C10K; the current fixed thread pool is
  connection-bound (noted in `docs/decisions/tier-4.3.md`).
- **TLS at the edge** (Caddy/nginx) so the gateway JWT isn't sent in clear text.

## Working agreement

Follow the loop in `CLAUDE.md`: plan mode → approval → implement with tests → `docs/decisions/tier-<n>.md`
→ hand back. Keep sub-tiers independently mergeable and the e2e green.
