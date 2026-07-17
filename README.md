# Mini Dynamo

[![CI](https://github.com/SnehilK3372/Mini_Dynamo/actions/workflows/ci.yml/badge.svg)](https://github.com/SnehilK3372/Mini_Dynamo/actions/workflows/ci.yml)

A distributed key-value store in C++17, modeled on [Amazon's Dynamo](https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf). Keys are placed on a **consistent-hashing ring with virtual nodes**; any node can accept a request and acts as **coordinator**, routing it to the key's owners. Writes are versioned with **vector clocks**, replicated to N owners, and acknowledged by a **write quorum (W)**; reads gather a **read quorum (R)**, reconcile versions, return the current value (or conflicting **siblings**), and **repair** stale replicas. Each node persists to its own **RocksDB** instance. Nodes find each other by **SWIM gossip**, which also detects and evicts dead nodes.

This README describes what the system does **today** (through Tier 4.5). The roadmap and the full architectural reasoning are in [docs/roadmap.md](docs/roadmap.md), [docs/build_plan.md](docs/build_plan.md) and [docs/full_arch.md](docs/full_arch.md); the per-tier decisions are in [docs/decisions/](docs/decisions/). **What actually limits the system today is catalogued honestly in [docs/scalability-constraints.md](docs/scalability-constraints.md).**

## Architecture

Each node runs a `TCPServer` that accepts framed connections and hands each request to the `Node`, which parses it and delegates to a `Coordinator`. A `Router` holds the consistent-hashing ring (virtual nodes, 64-bit hashing) and answers "which N physical nodes own this key?".

- **PUT** forwards to the key's primary owner, which coordinates: it builds a new vector clock (its own entry bumped above what it already holds), writes the versioned value locally and to the other owners, and returns `OK` once **W** replicas acknowledge (else a retryable `quorum_not_met`).
- **GET** is coordinated by whichever node receives it: it reads from the N owners, waits for **R** responses, and compares their clocks. If one version dominates, it returns that (and asynchronously repairs any strictly-stale replica). If two versions are concurrent, it returns all **siblings** for the client to reconcile.
- Each replica persists the serialized `VersionedValue` to its own **RocksDB** directory — shared-nothing; nodes coordinate only over the network.

```
client ──HTTP──► nginx ──► gateway ×2  (hashes the key, routes to its primary owner)
                             │
                             ▼  TCP (framed, pooled)
                          coordinator node
                             │  Router: hash(key) → N owners
                             │  vector clock ── quorum (W acks / R responses) ── read repair
                             │  SWIM gossip: membership + failure detection
                             ▼
                  per-node RocksDB (durable, shared-nothing)
```

`N`, `W`, `R` default to `3, 2, 2` (so `W+R>N`: a read set and a write set always overlap) and are tunable per request.

Beyond the core, the cluster **gossips** its membership (SWIM, with suspicion and dead-node eviction), keeps writes available through outages with **hinted handoff**, **pools** inter-node connections, and **bounds** vector clocks by pruning. The gateway is stateless and horizontally scaled behind nginx, and routes each key **directly to its primary owner** rather than funnelling through one node.

## Running the cluster

Requires Docker. `docker compose up --build` starts 9 containers: 3 nodes (`node1`–`node3`), 2 gateway replicas behind **nginx on `:8080`**, Postgres, Prometheus, and Grafana.

```bash
docker compose up --build
```

A node is configured entirely by environment variables:

| Variable | Meaning |
|---|---|
| `NODE_ID` | Unique node name (also its hostname on the ring) |
| `NODE_PORT` | TCP port to listen on |
| `HOST` | Bind address (default `0.0.0.0`) |
| `SEED_NODES` | Comma-separated `host:port` seeds to gossip-join through; **omit to self-bootstrap the ring**. (`BOOTSTRAP_IP`/`BOOTSTRAP_PORT` still work as a single seed) |
| `ADVERTISE_HOST` | Hostname peers use to reach this node (default `NODE_ID`) |
| `VNODES` | Virtual nodes per physical node (default 128) — must match the gateway's `cluster.virtual-nodes` |
| `STORAGE_ENGINE` | `rocksdb` (default when compiled with RocksDB) or `memory` |
| `DATA_DIR` | RocksDB directory (default `/data/<NODE_ID>`) |
| `MAX_CLOCK_ENTRIES` | Vector-clock bound (default 20) |
| `WORKER_THREADS` / `POOL_MAX_CONNS_PER_PEER` | Server thread pool / per-peer connection pool |
| `GOSSIP_*` | SWIM period, ping timeout, K, suspicion multiplier |

Building outside Docker requires Linux (POSIX sockets): `cmake -S . -B build && cmake --build build -j4` produces `build/kvstore`. Unit tests build anywhere (see [Testing](#testing)).

## Wire protocol

Plain TCP, **length-prefixed framed** (`<byte-length>\n<payload>`), payload pipe-delimited text. Connections are **persistent** — a socket carries many framed request/response pairs (Tier 4.3). Values are **base64-encoded** so arbitrary bytes (including `|` and newlines) are carried safely; vector clocks serialize as `node1:3:1720000000000` (`node:counter:timestamp`, Tier 4.5 — the timestamp drives clock pruning and never affects causality).

| Request | Response |
|---|---|
| `PUT\|<key>\|<b64value>\|<origin>\|<N>\|<W>\|<R>\|<clock>` | `RESPONSE\|OK\|<clock>` or `RESPONSE\|ERROR\|<reason>` |
| `GET\|<key>\|<origin>\|<N>\|<R>` | `RESPONSE\|OK\|<b64value>\|<clock>`, `RESPONSE\|SIBLINGS\|<n>\|<b64v>\|<clk>...`, `RESPONSE\|NOTFOUND`, or `RESPONSE\|ERROR\|<reason>` |
| `DELETE\|<key>\|<origin>\|<N>\|<W>\|<clock>` | `RESPONSE\|OK\|<clock>` — a versioned **tombstone**, not a local erase |
| `REPLICATE\|<key>\|<b64value>\|<origin>\|<clock>` | `RESPONSE\|OK` (internal, coordinator→replica) |
| `READ\|<key>\|<origin>` | `VAL\|<b64value>\|<clock>` or `VAL\|NOTFOUND` (internal) |
| `RING\|<origin>` | `RING\n<count>\n<id>\|<host>\|<port>\n...` — read-only ring snapshot (the gateway polls this to route) |
| `SWIM_PING` / `SWIM_PING_REQ` / `SWIM_ACK` / `SWIM_JOIN` | gossip membership + failure detection (internal, Tier 4.1) |
| `JOIN\|<node_id>\|<value>\|<origin>\|<host>\|<port>` | `RING_UPDATE\n<count>\n<id>\|<host>\|<port>\n...` — legacy bootstrap join; gossip is the live path |

## Testing

The suite covers the classic pyramid, and the whole thing runs in CI on every push and pull request (see the badge at the top).

**Unit**
- **C++ (GoogleTest):** ring/hashing, vector-clock comparison, quorum arithmetic, base64, versioned-value/tombstone round-trips, RocksDB persistence.
  `cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build -j4 && ctest --test-dir build`
- **Java (JUnit 5 + Mockito):** the gateway service layer with the cluster mocked (incl. the 404/409/502/503 error paths), JWT issue/validate, and the cluster wire codec.
  `cd gateway && ./mvnw test`

**Integration (Testcontainers — Docker required)**
- `GatewayIntegrationTest` drives the HTTP API (REST Assured) against a real PostgreSQL and a framed fake cluster; `RealClusterIT` does the same against the **actual C++ node image**, proving the real on-the-wire protocol.
  `docker build -t mini-dynamo-node:ci . && cd gateway && NODE_IMAGE=mini-dynamo-node:ci ./mvnw verify`

**End-to-end (whole stack, one command)**
- `scripts/e2e.sh` brings up the full `docker compose` stack, writes with `W=2`, kills a node and reads with `R=2` to prove **availability under one failure**, then restarts the node and reads to prove **convergence via read repair**.
  `scripts/e2e.sh`

## Deployment (AWS)

The whole stack runs on a single EC2 instance via the existing `docker compose` file,
with **only nginx (`:8080`) exposed** — the gateway replicas, Grafana, Prometheus, Postgres
and the nodes all stay private (Grafana is reached over an SSH tunnel). The operational
runbook is [`docs/runbooks/ec2-deploy.md`](docs/runbooks/ec2-deploy.md); instance sizing,
security-group rules and the manual redeploy workflow are in
[`deploy/aws/README.md`](deploy/aws/README.md). Multi-host Swarm lives in [`deploy/swarm/`](deploy/swarm/).

```bash
# on a fresh Amazon Linux 2023 / Ubuntu m7i-flex.large (>=30 GB disk):
curl -fsSL https://raw.githubusercontent.com/SnehilK3372/Mini_Dynamo/main/deploy/aws/bootstrap.sh | bash
# then set real secrets in ~/Mini_Dynamo/.env and: docker compose up -d
```

> **Upgrading an existing box to Tier 4.4+ needs a fresh cluster.** 4.4 changed the ring hash
> and 4.5 the vector-clock wire format, so old data is unreachable and all nodes must run one
> build: `docker compose down -v` before bringing the new version up.

Hitting the public API (see the runbook for the JWT flow and Swagger UI at
`/swagger-ui.html`):

```bash
TOKEN=$(curl -s -X POST http://<host>:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<AUTH_PASSWORD>"}' | jq -r .token)
curl http://<host>:8080/v1/kv/hello -H "Authorization: Bearer $TOKEN"
```

Redeploys are manual — **Actions → "Deploy to EC2 (manual)"** (no auto-deploy on merge).
nginx serves plain HTTP (JWT unencrypted in transit) — fine for a demo; terminate TLS there
if you keep it up. `m7i-flex.large` is **not** free-tier — stop the instance when idle.

## Scaling (Tier 4)

Four ceilings were removed in order; each has a design record in [docs/decisions/](docs/decisions/):

- **4.1 — SWIM gossip membership.** Replaces JOIN-only membership: every node converges on the same ring, and dead nodes are suspected and evicted automatically.
- **4.2 — Hinted handoff + Merkle anti-entropy.** Sloppy quorum keeps writes available when an owner is down; hints are delivered on recovery.
- **4.3 — Connection pooling, thread pool, liveness-aware reads.** Persistent pooled sockets, bounded handler threads, and reads that skip known-dead replicas instead of stalling on them.
- **4.4 — Horizontal gateways + ring-aware routing.** Stateless gateway replicas behind nginx; each hashes the key locally (a portable hash shared bit-for-bit with the C++ ring) and routes straight to its primary owner, spreading coordination across all nodes.
- **4.5 — Bounded vector clocks + multi-host Swarm.** Clocks carry timestamps and are pruned to a bound; `deploy/swarm/` scales one `kvstore` service across hosts for the scaling benchmark.

## Honest status

- **The "scales to thousands" claim is unproven.** The 5→100 benchmark harness exists (`bench/scale/`), but the multi-host run hasn't happened — [`bench/scale/RESULTS.md`](bench/scale/RESULTS.md) is deliberately empty rather than filled with estimates.
- **Anti-entropy is not wired across nodes.** The Merkle tree is implemented and unit-tested, but the cross-node exchange is still stubbed, so `antientropy_syncs_total` stays 0. Read repair and hinted handoff are the convergence mechanisms that actually run.
- Everything that limits the system today — including single points of failure and where it would break first — is catalogued in **[docs/scalability-constraints.md](docs/scalability-constraints.md)**.

## Future work

- Wire the `MERKLE_*` exchange so anti-entropy actually repairs rarely-read keys (the largest correctness gap).
- Per-replica sibling storage; pool the forward-to-primary and gossip paths; multiple seeds; TLS at the edge.
- Async (epoll) server if true C10K per node is ever needed — the thread pool is connection-bound by design.
