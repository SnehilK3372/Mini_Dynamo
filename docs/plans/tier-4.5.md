# Tier 4.5 — Multi-Host Deploy + Vector-Clock Pruning + Scale Benchmark

## Context

Tiers 4.1–4.4 removed the membership, durability, transport, and gateway ceilings — all validated on a
single Docker host. Two gaps remain before the "scales to thousands" claim is credible:

1. **Unbounded vector clocks.** `VectorClock` (`src/vector_clock.h`) grows one entry per node that ever
   coordinates a key. At thousands of nodes a hot key's clock grows without bound, bloating every
   value, every replica message, and every comparison.
2. **Never proven across real hosts.** Everything so far runs on one box's bridge network. Gossip,
   hinted handoff, and ring-aware routing have never crossed a real network boundary, and there is no
   scaling curve to back the thesis.

Tier 4.5 closes both: bound the clocks, deploy across a Docker Swarm of 3–5 hosts, and produce a
5→100-node scaling benchmark.

**Decisions taken:** full scope (Swarm multi-host + pruning + full benchmark), and Dynamo-faithful
timestamp-based pruning — accepting a **breaking wire/storage format change** for the clock.

## Approach

### A. Vector clock with timestamps + pruning (`src/vector_clock.{h,cpp}`)

`counts_` becomes `std::map<std::string, Entry>` where `Entry { uint64_t counter; uint64_t updated_ms; }`.
`counts_` is fully encapsulated (nothing outside `vector_clock.cpp` touches it; `entries()` is unused),
so the blast radius is this one class.

- **Wire format (breaking):** `node:counter:ts`, comma-separated, still sorted/canonical. `parse()`
  splits on the *first* colon for the node id (ids may not contain `:` or `,` — already documented),
  then `counter:ts`. Parsing stays **lenient**: a legacy 2-field `node:counter` yields `ts=0` (treated
  as oldest → pruned first), so a stray old value degrades gracefully instead of throwing.
- **Timestamps never affect causality.** `compare()` keeps using **counters only** — timestamps are
  pruning metadata, not causal information. Likewise `operator==` compares **counters only**, so two
  causally-identical clocks stay equal regardless of timestamps (the coordinator's sibling dedup in
  `maximalVersions` relies on `compare(...) == EQUAL`).
- `set(node, counter)` / `increment(node)` stamp `updated_ms = now`; an explicit
  `set(node, counter, ts)` overload is used by `parse()`. `merge()` takes the element-wise max counter
  and carries the *winning* entry's timestamp.
- **`prune(size_t max_entries)`**: if `size() > max_entries`, drop entries with the oldest `updated_ms`
  until `size() == max_entries`; ties broken by node id so pruning is deterministic given identical
  input (all nodes prune the same clock the same way).
- Called from `Coordinator::bumpedClock()` (`src/coordinator.cpp`) after the bump, before the value is
  stored/replicated. Bound via `MAX_CLOCK_ENTRIES` (default **20**, env-configurable in `main.cpp`).

**Honest correctness note (corrects the original plan's claim).** Pruning is *not* strictly loss-free.
The common case degrades to a false **CONCURRENT** (siblings surfaced — conservative, no loss). But a
false **A_DOMINATES** is possible in a contrived case: if `B={x:5}` is pruned to `B'={}` while
`A={x:1}` retains `x`, `A` now appears to dominate `B`. This requires the pruned clock to lose *every*
entry that carried its dominance, which a generous `MAX_CLOCK_ENTRIES` makes vanishingly unlikely
(a pruned clock still retains 20 entries). The Dynamo paper makes and documents the same trade
("truncation can lead to inefficiencies… descendant relationships cannot be derived accurately").
This will be stated plainly in the decisions log rather than papered over.

### B. Multi-host Swarm (`deploy/swarm/`)

The blocker: today `docker-compose.yml` hand-declares `node1/node2/node3` as three separate services
with distinct `NODE_ID`/ports. That cannot scale to 100. The Swarm stack replaces them with **one
`kvstore` service scaled to N replicas**, each deriving a unique identity from Swarm's env templating:

```yaml
environment:
  NODE_ID: "node{{.Task.Slot}}"        # unique per replica
  ADVERTISE_HOST: "{{.Task.Name}}"     # per-task hostname, resolvable on the overlay
  NODE_PORT: "5001"                    # same port for all — each task has its own overlay IP
  SEED_NODES: "tasks.kvstore:5001"     # Swarm DNS → all task IPs; gossip seeds off one
```

`main.cpp` already reads exactly these env vars (`NODE_ID`, `ADVERTISE_HOST`, `NODE_PORT`,
`SEED_NODES`) — **no C++ change needed** for multi-host. Overlay network; gateway replicas + nginx +
postgres + prometheus + grafana carried over. Prometheus discovers nodes via `dns_sd_configs` on
`tasks.kvstore:9100` (same pattern already used for gateway replicas in 4.4).

- `deploy/swarm/docker-stack.yml` — the stack
- `deploy/swarm/init-swarm.sh` — init manager + join workers
- `deploy/swarm/deploy.sh` — `docker stack deploy`

### C. Scale benchmark (`bench/scale/`)

- `bench/scale/k6_uniform.js` — uniform 70/30 read/write over a fixed keyspace (adapted from
  `bench/load.js`; reuse its auth/setup and `AUTH_USER`/`AUTH_PASS` handling).
- `bench/scale/scale_test.sh` — for N in 5,10,20,50,100: `docker service scale kvstore=N`, wait for
  gossip convergence (poll `RING` until it reports N), run k6, capture throughput/p50/p95/p99 plus
  convergence time, failure-detection time, and hint-delivery time from Prometheus.
- `bench/scale/RESULTS.md` — the curve + analysis.
- `tests/multi_host_smoke.sh` — cross-host write/read, kill a host, verify availability + hint delivery.

## Files

- **Modified:** `src/vector_clock.{h,cpp}` (Entry + timestamps + `prune`), `src/coordinator.cpp`
  (call `prune()` in `bumpedClock`), `src/main.cpp` (`MAX_CLOCK_ENTRIES` env),
  `tests/vector_clock_test.cpp` (+pruning/format tests), `docs/roadmap.md` (mark 4.5 done)
- **New:** `deploy/swarm/{docker-stack.yml,init-swarm.sh,deploy.sh}`,
  `bench/scale/{scale_test.sh,k6_uniform.js,RESULTS.md}`, `tests/multi_host_smoke.sh`,
  `docs/decisions/tier-4.5.md`, `docs/plans/tier-4.5.md`

## Tests

- **`tests/vector_clock_test.cpp`**: round-trip the new `node:counter:ts` format; legacy `node:counter`
  parses with `ts=0`; `compare()`/`operator==` ignore timestamps (causally-equal clocks with different
  ts are EQUAL); 25 entries → `prune(20)` keeps the 20 newest and is deterministic; a pruned clock
  yields CONCURRENT (not a wrong dominance) in the realistic scenario.
- Existing suites must stay green — especially `coordinator_test` (clock bumping) and
  `versioned_value_test` (clock embedded in the serialized value).
- **`tests/multi_host_smoke.sh`**: cross-host PUT/GET; kill a host; reads stay available; hints
  delivered on recovery.

## Verification

1. **Unit**: `cmake -S . -B build -DBUILD_TESTING=ON -DWITH_PROMETHEUS=OFF && cmake --build build &&
   ctest --test-dir build` green (build/run in WSL as in prior tiers). Gateway suite unaffected (it
   treats the clock as an opaque string — confirmed in `ClusterClient`).
2. **Single-host regression first**: `scripts/e2e.sh` + `bench/chaos.sh` still pass on plain compose
   with pruning enabled — proves the format change didn't break the existing stack before touching
   Swarm.
3. **Clock bound**: drive writes for one key through many coordinators; assert the stored clock never
   exceeds `MAX_CLOCK_ENTRIES` (read it back via GET and count entries).
4. **Multi-host**: `deploy/swarm/init-swarm.sh` + `deploy.sh` on 3–5 hosts; `tests/multi_host_smoke.sh`
   passes; gossip converges across hosts.
5. **Scaling curve**: `bench/scale/scale_test.sh` for 5→100; record in `bench/scale/RESULTS.md`.
   Success = near-linear throughput, bounded p99, gossip convergence within `O(log N)·T`, failure
   detection within the suspicion timeout regardless of N.

## Sequencing & risk

Order: **(A) pruning → single-host regression → (B) Swarm stack → (C) benchmark.** (A) is
independently mergeable and CI-verifiable; if the infra work stalls, the pruning still lands. The tier
can be split at the (A)/(B) boundary into two PRs if review gets unwieldy.

- **Breaking clock format** — old stored values parse with `ts=0`; all nodes must run one build. Deploy
  all-at-once on a fresh cluster (same posture as 4.4's ring reshuffle). Both breaking changes land
  together in the next EC2 redeploy.
- **Pruning correctness** — see the honest note in (A); mitigated by a generous default and documented.
- **Swarm identity** depends on `{{.Task.Slot}}`/`{{.Task.Name}}` templating; if a Swarm version
  doesn't expand these, fall back to a per-replica entrypoint deriving `NODE_ID` from the hostname.
- **Cost** — 3–5 EC2 hosts running a 100-node benchmark is real spend; `deploy.sh`/teardown are scripted
  so the cluster isn't left running.
