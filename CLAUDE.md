# CLAUDE.md — Mini Dynamo

## How we work on this project (read this first)

**Build one tier at a time.** The original and complete tiered build plan is in `@docs/full_arch.md` and the actual plan we're going to implement is in focused `@docs/focused_arch.md` and`@docs/build_plan.md`. Read full_arch.md first to get an overall idea and implement the tiers discussed in focused_arch.  Do not start tier N+1 until I say so.

**Every tier follows the same loop:**

1. Enter plan mode. Read the relevant tier from `docs/build-plan.md` in full and produce an implementation plan. Wait for my explicit approval before writing any code.
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

Mini Dynamo — a distributed key-value store in C++17, modeled on Amazon Dynamo. Keys are placed on a consistent-hashing ring; each node stores its own shard and replicates writes to peers. See `@docs/architecture.md` for the full design (target end-state) and `@docs/build-plan.md` for the tiered path from current code to there. **Read the relevant tier before implementing it.**

## Stack (do not deviate without flagging)

- Core: C++17, RocksDB per-node storage (once Tier 1A lands), raw TCP inter-node
- Gateway (Tier 1B): Spring Boot / Java 17, JWT auth, PostgreSQL metadata via JPA
- Observability: Prometheus + Grafana; structured JSON logs to stdout
- CI: GitHub Actions. Deploy: Docker Compose local, single AWS EC2 for the live demo.

## Build, run, test

```bash
docker-compose up --build         # canonical: 3-node cluster (node1 bootstrap :5001, node2/3 join)
cmake . && make -j4               # direct build inside Linux — produces ./kvstore
```

POSIX sockets — Linux/Docker only, no native Windows build.

A node is configured entirely via env vars (see `main.cpp`): `NODE_ID`, `NODE_PORT`, `HOST` (default `0.0.0.0`), and for non-bootstrap nodes `BOOTSTRAP_IP` + `BOOTSTRAP_PORT`. A node is the bootstrap iff `BOOTSTRAP_IP` is empty.

**No automated tests exist yet** — `tests.txt` is manual `netcat` scripts. Standing up GoogleTest is part of Tier 1A. Until then, exercise manually: `printf "PUT|k1|v1|cli" | nc node1 5001`.

## Ground truth about the current code (before you touch it)

**Stale build artifacts — ignore them.** The root `Makefile`, `CMakeFiles/`, and `build/` are stale and hardcoded to a Linux author path (`/home/senku/Desktop/CN Project`). Only `CMakeLists.txt` is source of truth; regenerate with `cmake .`. Tier 0 removes them from history.

**Request flow** (client `PUT`/`GET` hitting any node):

1. `TCPServer` (`src/net/tcp_server.cpp`) accepts, does a single `read()` of ≤4096 bytes, deserializes into a `Message`, calls `Node::onMessageReceived`.
2. `Router` (`src/router.cpp`) is the consistent-hashing ring. `findOwners(key, N)` hashes with `hash64` and walks clockwise to return N distinct physical owners.
3. `Node` (`src/node.cpp`) dispatches by type:
   - **PUT** — if primary owner, store locally then fire-and-forget `REPLICATE` to the other `REPLICATION_FACTOR` (=3) owners; else forward to primary and relay reply.
   - **GET** — walk owners, return the first that has the key; else `NOTFOUND`.
   - **REPLICATE** — store locally.
   - **JOIN** — reply `RING_UPDATE` listing current ring, then add joiner locally.

**Bootstrap.** Startup lives in `main.cpp`, NOT `Node::start()`. Joining node sends `JOIN` to bootstrap, parses `RING_UPDATE`, adds each peer to its `Router`. No gossip, no dead-node removal.

**Wire protocol** (pipe-delimited, `src/message.cpp`):
- Request: `TYPE|KEY|VALUE|ORIGIN`; `JOIN` appends `|HOST|PORT`
- Response: `RESPONSE|OK|<value>`, `RESPONSE|NOTFOUND`, `RESPONSE|ERROR|<reason>`, or `RING_UPDATE\n<count>\n<id>|<host>|<port>\n...`
- **Keys/values must not contain `|`** — no escaping.

**Framing mismatch (important when editing networking):** `TCPServer` reads unframed; `Node::sendMessage`/`TCPClient` send unframed. The length-prefixed `tcp_send_recv` path exists but is bypassed (`use_length_prefix=false`). Real traffic is single unframed reads.

**Dead / misleading code — do not reason from these; Tier 0 removes them:**
- `Node::start()` and `Node::server_loop()` — never called; `main.cpp` runs its own JOIN + `TCPServer`. `start()` also reads different env vars (`SEED_HOST`/`SEED_PORT`) than the live path.
- `store` / `store_mtx` in `node.h` — unused second map; live store is `local_storage` / `storage_mutex`.
- `hashKey` (FNV) in `src/hash.cpp`/`hash.h` — unused; ring uses `hash64` from `src/util.h`.
- `config/node_config.json` — not read anywhere.
- `<nlohmann/json.hpp>` included by `node.h` and installed in the Dockerfile but unused.