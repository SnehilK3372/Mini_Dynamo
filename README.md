# Mini Dynamo

A distributed key-value store in C++17, modeled on [Amazon's Dynamo](https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf). Keys are placed on a **consistent-hashing ring with virtual nodes**; any node can accept a request and acts as **coordinator**, routing it to the key's owners. Writes are versioned with **vector clocks**, replicated to N owners, and acknowledged by a **write quorum (W)**; reads gather a **read quorum (R)**, reconcile versions, return the current value (or conflicting **siblings**), and **repair** stale replicas. Each node persists to its own **RocksDB** instance. New nodes join through a bootstrap handshake.

This README describes what the system does **today** (through Tier 1A). The roadmap and the full architectural reasoning are in [docs/build_plan.md](docs/build_plan.md) and [docs/full_arch.md](docs/full_arch.md); the per-tier decisions are in [docs/decisions/](docs/decisions/).

## Architecture

Each node runs a `TCPServer` that accepts framed connections and hands each request to the `Node`, which parses it and delegates to a `Coordinator`. A `Router` holds the consistent-hashing ring (virtual nodes, 64-bit hashing) and answers "which N physical nodes own this key?".

- **PUT** forwards to the key's primary owner, which coordinates: it builds a new vector clock (its own entry bumped above what it already holds), writes the versioned value locally and to the other owners, and returns `OK` once **W** replicas acknowledge (else a retryable `quorum_not_met`).
- **GET** is coordinated by whichever node receives it: it reads from the N owners, waits for **R** responses, and compares their clocks. If one version dominates, it returns that (and asynchronously repairs any strictly-stale replica). If two versions are concurrent, it returns all **siblings** for the client to reconcile.
- Each replica persists the serialized `VersionedValue` to its own **RocksDB** directory — shared-nothing; nodes coordinate only over the network.

```
client ──TCP(framed)──► coordinator
                          │  Router: hash(key) → N owners
                          │  vector clock ── quorum (W acks / R responses) ── read repair
                          ▼
               per-node RocksDB (durable, shared-nothing)
```

`N`, `W`, `R` default to `3, 2, 2` (so `W+R>N`: a read set and a write set always overlap) and are tunable per request.

## Running the cluster

Requires Docker. The compose file starts a 3-node cluster: `node1` (bootstrap, port 5001), `node2` (5002), `node3` (5003).

```bash
docker-compose up --build
```

A node is configured entirely by environment variables:

| Variable | Meaning |
|---|---|
| `NODE_ID` | Unique node name (also its hostname on the ring) |
| `NODE_PORT` | TCP port to listen on |
| `HOST` | Bind address (default `0.0.0.0`) |
| `BOOTSTRAP_IP` / `BOOTSTRAP_PORT` | Existing node to join through; **omit both to start as the bootstrap node** |
| `STORAGE_ENGINE` | `rocksdb` (default when compiled with RocksDB) or `memory` |
| `DATA_DIR` | RocksDB directory (default `/data/<NODE_ID>`) |

Building outside Docker requires Linux (POSIX sockets): `cmake -S . -B build && cmake --build build -j4` produces `build/kvstore`. Unit tests build anywhere (see [tests.txt](tests.txt)).

## Wire protocol

Plain TCP, **length-prefixed framed** (`<byte-length>\n<payload>`), payload pipe-delimited text. Values are **base64-encoded** so arbitrary bytes (including `|` and newlines) are carried safely; vector clocks serialize as `node1:3,node2:1`.

| Request | Response |
|---|---|
| `PUT\|<key>\|<b64value>\|<origin>\|<N>\|<W>\|<R>\|<clock>` | `RESPONSE\|OK\|<clock>` or `RESPONSE\|ERROR\|<reason>` |
| `GET\|<key>\|<origin>\|<N>\|<R>` | `RESPONSE\|OK\|<b64value>\|<clock>`, `RESPONSE\|SIBLINGS\|<n>\|<b64v>\|<clk>...`, `RESPONSE\|NOTFOUND`, or `RESPONSE\|ERROR\|<reason>` |
| `REPLICATE\|<key>\|<b64value>\|<origin>\|<clock>` | `RESPONSE\|OK` (internal, coordinator→replica) |
| `READ\|<key>\|<origin>` | `VAL\|<b64value>\|<clock>` or `VAL\|NOTFOUND` (internal) |
| `JOIN\|<node_id>\|<value>\|<origin>\|<host>\|<port>` | `RING_UPDATE\n<count>\n<id>\|<host>\|<port>\n...` |

See [tests.txt](tests.txt) for framed `netcat` examples and the automated test commands.

## Future work

Named so the roadmap is honest, not as gaps in the current tier:

- **Hinted handoff** (stay writeable when an owner is down at write time) and **Merkle-tree anti-entropy** (repair rarely-read keys) — completing the AP convergence story beyond read repair. Per-replica sibling storage lands with these.
- **Gateway** (Spring Boot REST + JWT + PostgreSQL metadata), **observability** (Prometheus/Grafana, structured logs), **CI**, load/chaos testing, and AWS deployment — Tiers 1B onward.
- Gossip-based membership and dead-node removal (membership is currently learned only at JOIN).
