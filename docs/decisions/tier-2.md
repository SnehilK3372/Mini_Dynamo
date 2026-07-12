# Tier 2 — Load + chaos testing (decisions log)

## What was built

A small benchmark harness under `bench/` that produces **measured** numbers instead of
adjectives:
- `bench/load.js` — a k6 script: JWT auth in `setup()`, then a PUT/GET mix over a fixed key
  pool, parameterized on `N/W/R` (query params) and on VUs/duration/write-ratio. PUT and GET
  latencies are tracked in separate trends so their percentiles report apart; throughput is
  k6's `http_reqs` rate. No remote imports — runs fully offline.
- `bench/run.sh` — runs one profile as a `grafana/k6` container on the compose network,
  hitting `gateway:8080`.
- `bench/chaos.sh` — one command: seed keys, drive a background read load, kill a node
  mid-load, overwrite keys while it's down, restart it, and re-read; asserts reads stayed
  available through the kill and that `read_repair_total` rose (convergence).
- `bench/RESULTS.md` — the actual figures, honestly framed.

## Key design choices (and the rejected alternative for each)

1. **k6 over JMeter.** The load model *is* code — a small JS file that lives in the repo,
   diffs cleanly, takes parameters as env vars, and runs headless in a container (or CI) with
   no GUI and no XML. Its summary reports the percentiles we care about (p50/p95/p99) out of the
   box. *Rejected:* JMeter — a heavyweight GUI tool whose `.jmx` plans are verbose XML that
   review poorly and whose percentile reporting needs extra listeners/plugins. For a
   code-reviewed portfolio, a scriptable tool wins.

2. **Drive the load through the gateway, not the cluster's TCP protocol directly.** The
   benchmark measures the system as a client actually uses it — HTTP + JWT + the coordinator
   hop — so the numbers reflect the real end-to-end path. *Rejected:* a raw-socket load
   generator against a node — it would post prettier numbers by skipping the gateway, but they
   wouldn't describe anything a user experiences.

3. **A read-only background load for the chaos test, asserting *high* availability (>95%), not
   zero errors.** The availability thesis this store can honestly make is about *reads* under a
   single node loss (a read is coordinated by any live node and needs only R replicas). So the
   background load reads, and the assertion is that >95% of reads are served through the kill —
   killing a node mid-load legitimately drops a handful of in-flight requests, so demanding
   literally zero would be asserting something false. Convergence is shown by overwriting keys
   while the node is down and watching `read_repair_total` rise once it returns. *Rejected:* a
   mixed read/write chaos load asserting zero errors — it would fail on exactly the writes the
   system openly doesn't guarantee, conflating a known limitation with a regression.

   Building this test surfaced two real behaviours worth stating (see `bench/RESULTS.md`):
   under concurrent load with a node down, even a **live-primary `W=2` write** often can't get
   its second ack inside the 500 ms deadline (one spare replica, and it's busy) and returns a
   retryable `503` — so the chaos writes use `W=1` to create staleness reliably. And read repair
   fires from the **ambient read load itself**, not only an explicit sweep, so convergence is
   measured across the whole scenario rather than in a late window.

## What the numbers say — throughput vs quorum tightness (W=1 vs W=2)

Measured on this box (see `bench/RESULTS.md` for the full table), same 50-VU load:

- **W=2** (wait for a second replica ack): ~**805 req/s**, PUT p50 ~**17.9 ms**, and **0.43%** of
  writes rejected (`quorum_not_met`) under saturation.
- **W=1** (the coordinator's own write is enough): ~**850 req/s** (~**5–6% higher**), PUT p50
  ~**14.8 ms** (lower), and only **0.10%** rejected.

The shape is exactly what the model predicts: a `W=2` write can't return until a *second* node
acknowledges, so it carries that extra round trip in its latency and, under load, occasionally
misses the coordinator's deadline and returns a retryable rejection. `W=1` commits on the
coordinator alone — faster and almost never rejected — but it gives up the `W+R>N` overlap that
guarantees a subsequent quorum read sees the write, and it widens the window where a coordinator
crash loses an unreplicated write. The benchmark makes the **tunable-consistency knob concrete**:
you can buy throughput and tail-latency headroom with durability, per request.

## Where these numbers mislead (honest caveats)

- **Single-host Docker on a laptop.** All three nodes, the gateway, and Postgres share one
  machine's CPU and one Docker Desktop VM, so they contend rather than scale out. Treat the
  absolute req/s as a floor and the **comparisons** (W=1 vs W=2, VU scaling) as the real signal.
- **The tail is dominated by contention, not the algorithm.** p99 climbs to ~1 s at 50 VUs on
  this host — that's coordinator deadline + retries + JVM/GC + Docker overhead under saturation,
  not an inherent cost of quorum. On dedicated nodes the tail would be far tighter.
- **Read-heavy mix (30% writes).** Typical for a KV store, but a write-heavy workload would lean
  harder on the replication fan-out and show the W knob's cost more sharply.
- **No warm-up/tuning.** RocksDB caches, JIT warm-up, and connection pools aren't pre-warmed;
  these are cold-ish steady-state numbers, not a tuned peak.

## Scaling limits: how far can this actually go?

"3 nodes feels small" is a fair reaction, so here is the honest ceiling. "Heavier" splits into
two very different questions:

**More client load (higher req/s).** Feasible now — the bottleneck in these runs was the *host*
(three nodes + gateway + Postgres + Prometheus contending for one laptop under Docker Desktop),
not the design. On real hardware with a distributed load generator (k6 Cloud or several k6
runners) the same cluster posts higher, cleaner numbers. The hard ceiling on client throughput is
the **single gateway** (one JVM/Tomcat), independent of node count.

**More nodes (tens, hundreds, 1000+).** *Not* possible with the system as built — and this is an
architecture limit, not a benchmark one. The blocker is **membership**:

- Membership is learned **only at JOIN**; there is no gossip. A joiner contacts the single
  bootstrap, gets a snapshot of the ring *as the bootstrap knows it at that instant*, and stops.
- So **ring views diverge and only the bootstrap is ever complete.** `node2` learns `{node1}` and
  never learns `node3`, `node4`, … that join later. This is real even at N=3: `node2`'s ring is
  missing `node3`. The cluster routes correctly today only because the gateway hits `node1` (the
  bootstrap, full ring) first and it coordinates — a non-bootstrap coordinator can misroute.
- There is **no dead-node removal, no rebalancing, and no anti-entropy** (Merkle trees are named
  future work); the single bootstrap is also a join serialization point and a SPOF.

**Realistic ceiling as-is:** ~3–7 nodes for correct routing (bounded by "only the bootstrap has
the full ring"), with client throughput capped by the single gateway. Benchmarking at 1000 nodes
would be measuring a cluster that cannot correctly *form*, so it isn't a meaningful test — the
prerequisite is convergent membership.

**What unlocks real scale** (sketched as **Tier 4** in `docs/build_plan.md`): gossip/SWIM
membership so every node converges on the full ring and detects failures (the load-bearing
change), then hinted handoff + Merkle anti-entropy, node-to-node connection pooling, and
horizontal gateways behind a load balancer — deployed across hosts (EKS/StatefulSets), with a
distributed load test to prove the scaling curve. For a portfolio, being able to *state* this
limit and its fix is worth more than brute-forcing node count: the Dynamo principles (tunable
quorums, vector clocks, read repair, availability under failure) are already demonstrated with
real numbers at 3–5 nodes.
