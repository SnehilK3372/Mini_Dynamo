# Mini Dynamo — Benchmark Results

Real numbers from `bench/load.js` (k6) and `bench/chaos.sh`, driven through the
gateway's HTTP+JWT API against the full `docker compose` stack.

## Environment (read the numbers honestly)

- **Single host:** all three C++ nodes, the gateway, PostgreSQL, Prometheus, and
  Grafana run as containers on **one Windows 11 laptop under Docker Desktop** (WSL2
  backend). They contend for the same CPU and one Docker VM — this is *not* a
  scaled-out deployment.
- So treat absolute throughput as a **floor**, and read the **comparisons** (W=1 vs
  W=2, VU scaling) and the **availability** result as the real signal — those hold
  regardless of the host.
- Workload: 30% PUT / 70% GET over a 200-key pool (`bench/load.js`), N=3, R=2 unless
  noted. Latencies in milliseconds. Generated with `grafana/k6` on the compose network.

Reproduce: `docker compose up -d && N=3 W=2 R=2 VUS=50 DURATION=30s bench/run.sh`.

## Throughput & latency (N=3, R=2)

| Profile | W | VUs | Throughput | GET p50 | GET p95 | GET p99 | PUT p50 | PUT p95 | PUT p99 | Writes rejected |
|---|---|---|---|---|---|---|---|---|---|---|
| baseline | 2 | 10 | **783 req/s** | 9.7 | 18.4 | 27.1 | 11.9 | 22.2 | 32.1 | 0.00% |
| higher load | 2 | 50 | **806 req/s** | 15.1 | 42.6 | ~1020 | 17.9 | 47.4 | ~1030 | 0.43% |
| loose quorum | 1 | 50 | **850 req/s** | 12.8 | 43.8 | ~1020 | 14.8 | 54.6 | ~1030 | 0.10% |

- **Checks succeeded:** 100% at 10 VUs; 99.86% (W=2) / 99.96% (W=1) at 50 VUs — the
  small miss is exactly the rejected writes below, not read failures.
- At 50 VUs the host saturates: median latency stays ~15 ms but the **p99 tail climbs to
  ~1 s** — coordinator deadline + retries + JVM/GC + Docker overhead under contention,
  not an algorithmic cost. On dedicated nodes the tail would be far tighter.

## Quorum tightness: W=1 vs W=2 (both at 50 VUs)

| Metric | W=2 | W=1 | Δ |
|---|---|---|---|
| Throughput | 806 req/s | 850 req/s | **+5.5%** |
| PUT p50 | 17.9 ms | 14.8 ms | **−17%** |
| Writes rejected (quorum_not_met) | 0.43% | 0.10% | **−4×** |

A `W=2` write can't return until a **second** replica acknowledges, so it carries an
extra round trip and, under saturation, occasionally misses the coordinator's deadline
and returns a retryable rejection. `W=1` commits on the coordinator alone — faster, far
fewer rejects — but gives up the `W+R>N` overlap that guarantees a quorum read sees the
write, and widens the window a coordinator crash could lose an unreplicated write. The
knob is real and measurable: **buy throughput/tail headroom with durability, per request.**

## Chaos: availability + convergence under a node kill (`bench/chaos.sh`)

Seed 8 keys → drive a 4-VU background **read** load → `docker stop node2` mid-load →
overwrite keys while it's down → `docker start node2` → converge. Measured result:

| Claim | Result |
|---|---|
| Reads served with a node down | **97.4%** — 574 errors / 22,028 reqs (2.60%) over the whole window incl. the kill instant |
| Writes accepted during the outage | **5/8** (`W=1`); the other 3 are keys primaried on the downed node → `502` |
| Read-repair convergence after restart | `read_repair_total` **15 → 21 (+6)** — node2 healed to the current versions |

**What this exposed (all honest, real behaviour):**
- **Reads stay highly available** under a single node loss — ~97% success even with the
  kill happening mid-load. The ~2–3% misses are in-flight requests at the instant the
  node dies plus a few `R=2` quorum slips under contention (one spare replica left).
- **Writes are only *partially* available:** a key whose **primary** is the downed node
  can't be written (forwarded to the dead primary → `502`; hinted handoff is deferred).
  And under concurrent load, even a live-primary **`W=2`** write struggles — the single
  remaining replica can't always ack inside the 500 ms deadline, so it returns a
  retryable `503`. The chaos writes use **`W=1`** to create staleness reliably; this is
  itself a finding about the durability/availability trade under degraded capacity.
- **Read repair converges the replica** once it returns — both the ambient read load and
  an explicit `R=3` sweep pull node2's stale copy into the comparison and heal it.

## How these were produced

- `bench/run.sh <label>` runs one k6 profile (env: `N W R VUS DURATION WRITE_RATIO`).
- `bench/chaos.sh` runs the kill-under-load scenario end to end (one command).
- Raw k6 summaries include per-op trends (`get_latency`, `put_latency`), `http_reqs`
  (throughput), `write_rejected`, and `op_errors`.
