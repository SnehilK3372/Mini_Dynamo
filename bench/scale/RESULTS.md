# Scaling benchmark — results

Produced by `bench/scale/scale_test.sh` against the Swarm stack
(`deploy/swarm/`). Companion to `bench/RESULTS.md` (single-host Tier 2 numbers).

> **Status: not yet run.** The harness, stack, and workload are in place; the
> numbers below get filled in from a real multi-host run. Nothing here is
> estimated or extrapolated — an empty table is honest, invented numbers are not.

## Setup (fill in when run)

| | |
|---|---|
| Swarm hosts | _e.g. 3 × m7i-flex.large (2 vCPU, 8 GB), same VPC/AZ_ |
| Node image | `mini-dynamo-node:latest` (RocksDB, prometheus-cpp) |
| Workload | `bench/scale/k6_uniform.js` — 70/30 read/write, uniform over 100k keys |
| Quorum | N=3, W=2, R=2 |
| k6 | VUS=___, DURATION=___ |
| Ring size | `kvstore` replicas + 1 (the bootstrap seed is a ring member) |

## Results

| Nodes | Convergence | Throughput | GET p99 | PUT p99 | 5xx |
|-------|-------------|------------|---------|---------|-----|
| 5     |             |            |         |         |     |
| 10    |             |            |         |         |     |
| 20    |             |            |         |         |     |
| 50    |             |            |         |         |     |
| 100   |             |            |         |         |     |

## What the numbers need to show

The Tier 4 thesis is that the architecture scales to thousands of nodes. This
benchmark cannot run thousands, so it argues by *shape* rather than by reaching
the target:

1. **Throughput rises with cluster size** (near-linear while the load generator
   and gateways aren't themselves the bottleneck). Ring-aware routing (Tier 4.4)
   is what makes this possible — coordination spreads across all nodes instead of
   funnelling through one.
2. **p99 stays bounded** as nodes are added. Quorum work per request is O(N=3)
   regardless of cluster size, so latency should not grow with the ring.
3. **Gossip convergence grows ~O(log N)**, not O(N) — the SWIM dissemination
   property from Tier 4.1. This is the single most important curve: it is what
   justifies extrapolating past 100.
4. **Failure detection is independent of cluster size** — bounded by the suspicion
   timeout (`suspicion_mult × protocol_period`), not by N.

If (1)–(4) hold from 5→100, the design extrapolates; if convergence grows
linearly, it does not, and that would be the honest finding to report.

## Caveats to record alongside any numbers

- **The load generator and gateway tier can saturate before the cluster does.**
  If throughput plateaus, check `nginx`/gateway CPU and k6's own host before
  concluding the cluster stopped scaling.
- **Nodes are co-located.** Many replicas per host share CPU and NIC, so
  per-node throughput at 100 nodes on 3 hosts is not comparable to 100 dedicated
  machines. The *shape* of the curve is the signal, not the absolute ops/s.
- **A 100-node run on 3–5 EC2 hosts costs real money.** Tear down with
  `deploy/swarm/deploy.sh down` (and stop the instances) as soon as the run ends.
