# CLAUDE.md — Mini Dynamo

## How we work on this project (read this first)

**Build one tier at a time.** The original and complete tiered build plan is in `@docs/full_arch.md` and the actual plan we're going to implement is in focused `@docs/focused_arch.md` and`@docs/build_plan.md`. Read full_arch.md first to get an overall idea and implement the tiers discussed in focused_arch.  Do not start tier N+1 until I say so.

**Every tier follows the same loop:**

1. Enter plan mode. Read the relevant tier from `docs/build_plan.md` (and `docs/roadmap.md` for Tier 4 onward) in full and produce an implementation plan. Wait for my explicit approval before writing any code.
2. Implement fully — production-quality code, meaningful comments explaining *why*, and tests (GoogleTest for C++, JUnit/Mockito for Java).
3. Run the tests yourself and iterate until green.
4. Write `docs/decisions/tier-<N>.md` with: what you built, the key design choices, the alternative you rejected and why for each, and one or two places the implementation could break under adversarial conditions. Tight, not padded.
5. Stop and hand back to me. Do NOT walk me through the code as you build — I will run a separate review + cross-questioning session later. Save teaching for then.

**Model choice by tier** (I set this, but flag if you think I've picked wrong):
- Fable 5 / Opus 4.8 → Tier 1A,1B and project structure (vector clocks, quorum, read repair) and any other reasoning-heavy work
- Sonnet 5 → most implementation tiers (gateway, observability, tests, CI)
- Haiku 4.5 → mechanical work only

**Working rhythm:** small reviewable steps. When you make a nontrivial design choice mid-tier, state the alternative you rejected and why in a comment or the decisions log. Never invent scope — if the tier plan doesn't call for it, don't add it.

## What this is

Mini Dynamo — a distributed key-value store in C++17, modeled on Amazon Dynamo. Keys are placed on a consistent-hashing ring; each node stores its own shard and replicates writes to peers. See `@docs/full_arch.md` for the full design (target end-state) and `@docs/build_plan.md` for the tiered path; `@docs/roadmap.md` tracks Tier 4 status and what's left. Per-tier decision records: `@docs/decisions/`. Approved plans: `@docs/plans/`. **Current, code-level limits: `@docs/scalability-constraints.md`.** **Read the relevant tier before implementing it.**

## Stack (do not deviate without flagging)

- Core: C++17, RocksDB per-node storage (once Tier 1A lands), raw TCP inter-node
- Gateway (Tier 1B): Spring Boot / Java 17, JWT auth, PostgreSQL metadata via JPA
- Observability: Prometheus + Grafana; structured JSON logs to stdout
- CI: GitHub Actions. Deploy: Docker Compose local, single AWS EC2 for the live demo.

## Build, run, test

```bash
docker compose up --build                        # canonical: 9 containers, API via nginx :8080
cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build -j4 && ctest --test-dir build
cd gateway && ./mvnw test                        # Java suite
scripts/e2e.sh                                   # whole-stack e2e, one command
```

POSIX sockets — the node binary is Linux/Docker only. `kv_core` + the unit tests build anywhere.

A node is configured entirely via env vars (see `main.cpp`): `NODE_ID`, `NODE_PORT`, `HOST`, `SEED_NODES` (omit to self-bootstrap), `ADVERTISE_HOST`, `VNODES`, `STORAGE_ENGINE`, `DATA_DIR`, `MAX_CLOCK_ENTRIES`, `WORKER_THREADS`, `POOL_*`, `GOSSIP_*`. Full table in `README.md`.

**Tests are real and CI-gated** (89 C++ / 31 Java + e2e, on every push). `tests.txt` was removed in Tier 1D. **CI pins `clang-format` 14** — format with `clang-format-14`, or CI's lint job fails on version drift.

**Local dev note:** this repo is checked out on Windows; the C++ toolchain lives in **WSL** (`g++`, no cmake) and `docker` is the Docker Desktop shim (needs Desktop running). Java/Maven run on the Windows side. Testcontainers tests can't reach the daemon from the Windows JVM, so `GatewayIntegrationTest` is CI-only locally.

## Ground truth about the current code (before you touch it)

*Accurate as of Tier 4.5. Everything Tier 0 flagged as stale/dead (`Makefile`, `CMakeFiles/`, `build/`, `src/hash.cpp`, `config/node_config.json`, `tests.txt`, `Node::start()`, nlohmann-json) has been **removed** — it no longer exists, don't go looking for it.*

**Request flow** (client → cluster):

1. **nginx** (`:8080`) round-robins to **2 stateless gateway replicas** (Spring Boot, JWT).
2. The **gateway** hashes the key with a Java port of `hash64` (`HashUtil`) against a locally-cached ring (`RingRouter`, refreshed every 5s by `RingPoller` via the `RING` command) and connects **directly to the key's primary owner** over a pooled socket. If its ring is empty/stale it falls back to the seed list — routing is an optimization; the receiving node always coordinates against its own ring, so this only ever costs a hop.
3. `TCPServer` (`src/net/tcp_server.cpp`) accepts and dispatches the connection to a fixed **ThreadPool**; the handler loops over **length-prefixed frames** on that socket until the peer closes it (connections are persistent).
4. `Node` (`src/node.cpp`) parses the frame and delegates to `Coordinator`; `Router` (`src/router.cpp`, `shared_mutex`, 128 vnodes) answers "which N physical nodes own this key?".
5. `Coordinator` (`src/coordinator.cpp`) runs the real quorum: fan out to N owners, `W` acks to commit / `R` responses to read, reconcile vector clocks, return the winner or **siblings**, and asynchronously **read-repair** stale replicas. Dead owners (per gossip) are **skipped on reads** and get a **hint** + stand-in on writes.

**Membership.** SWIM gossip (`src/gossip/`) — `SEED_NODES` (CSV) to join, then peer-to-peer probing with suspicion. Two kinds of departure, kept distinct (Dynamo's temporary-vs-permanent split): a **gossip-detected death** marks the node dead in Swim but **leaves it in the ring** (a transient failure must not reshuffle ownership; the coordinator skips it on reads, hints on writes), while an **administrative `LEAVE`** (Tier 4.6) tombstones it as `MemberState::Left` and **removes it from the ring** permanently. Since Tier 4.7, membership has **anti-entropy**: every ack carries a view digest, persistent mismatch triggers a `SWIM_SYNC` push-pull full-state exchange, and a **resurrection probe** pings one Dead member every 5 ticks — so a missed event (partition/drop) is a transient divergence, not a permanent one, and a healed partition revives without a restart. `BOOTSTRAP_IP`/`BOOTSTRAP_PORT` still work as a single seed. Startup lives in `main.cpp`.

**Wire protocol** — length-prefixed framing (`<len>\n<payload>`, `src/net/framing.cpp`), pipe-delimited fields, **base64-encoded values** (so values may contain any bytes). Verbs: `PUT`, `GET`, `DELETE` (tombstone), `REPLICATE`, `READ`, `RING`, `LEAVE` (administrative permanent removal, Tier 4.6), `SWIM_*` (incl. `SWIM_SYNC`/`SWIM_SYNC_ACK` membership anti-entropy, Tier 4.7). Legacy `JOIN` was **removed** in 4.7. Vector clocks serialize as `node:counter:timestamp`. **Node ids are `[A-Za-z0-9._-]{1,64}`, enforced at startup/join/event-apply (`isValidNodeId`, Tier 4.7).** Full table in `README.md`.

**Two breaking changes are live** — both require a **fresh cluster**, and all nodes must run one build:
- Tier 4.4 replaced `hash64`'s `std::hash` seed with FNV-1a (portable to Java) → the ring reshuffles.
- Tier 4.5 changed the clock wire form to `node:counter:ts` → old values reinterpret.

**Known gaps (don't rediscover these):** the Merkle **anti-entropy cross-node exchange is stubbed** in `main.cpp` (`antientropy_syncs_total` stays 0 — expected); the forward-to-primary and gossip paths are **not pooled**; the thread pool is connection-bound, not epoll. Current limits: `docs/scalability-constraints.md`.