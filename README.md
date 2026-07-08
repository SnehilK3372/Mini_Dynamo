# Mini Dynamo

A distributed key-value store in C++17, modeled on [Amazon's Dynamo](https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf). Keys are placed on a **consistent-hashing ring with virtual nodes**; any node can accept a request and acts as **coordinator**, routing it to the key's owners and replicating writes to peers over raw TCP. New nodes join the cluster through a bootstrap handshake.

This README describes what the system does **today**. The roadmap toward tunable quorum, vector clocks, read repair, and durable storage is in [docs/build_plan.md](docs/build_plan.md); the full architectural reasoning is in [docs/full_arch.md](docs/full_arch.md).

## Architecture (current state)

Each node runs a `TCPServer` that accepts connections and hands each message to the `Node`, which dispatches by type. A `Router` holds the consistent-hashing ring (3 virtual nodes per physical node, 64-bit hashing) and answers "which N physical nodes own this key?". On **PUT**, the receiving node looks up the primary owner: if that's itself, it stores the value locally (behind a pluggable `StorageEngine` — currently an in-memory map) and replicates to the other owners fire-and-forget; otherwise it forwards the request to the primary and relays the reply. On **GET**, it walks the owner list and returns the first hit. On **JOIN**, a bootstrap node replies with the current ring so the joiner can learn all existing peers. Membership is learned only at join time — there is no gossip or failure detection yet.

```
client ──TCP──► any node (coordinator)
                  │  Router: hash(key) → N owners on the ring
                  ├─ owner == self → StorageEngine (in-memory)
                  └─ else → forward/replicate to owner nodes ──► their StorageEngine
```

## Running the cluster

Requires Docker. The compose file starts a 3-node cluster: `node1` (bootstrap, port 5001), `node2` (5002) and `node3` (5003), which join via `node1`.

```bash
docker-compose up --build
```

A node is configured entirely by environment variables:

| Variable | Meaning |
|---|---|
| `NODE_ID` | Unique node name (also used as its hostname on the ring) |
| `NODE_PORT` | TCP port to listen on |
| `HOST` | Bind address (default `0.0.0.0`) |
| `BOOTSTRAP_IP` / `BOOTSTRAP_PORT` | Existing node to join through; **omit both to start as the bootstrap node** |

Building outside Docker requires Linux (the networking layer uses POSIX sockets): `cmake . && make -j4` produces `./kvstore`.

## Wire protocol

Plain TCP, pipe-delimited text. **Keys and values must not contain `|`** (no escaping) and requests must fit in a single 4 KB read.

| Request | Response |
|---|---|
| `PUT\|<key>\|<value>\|<origin>` | `RESPONSE\|OK` or `RESPONSE\|ERROR\|<reason>` |
| `GET\|<key>\|<origin>` | `RESPONSE\|OK\|<value>` or `RESPONSE\|NOTFOUND` |
| `REPLICATE\|<key>\|<value>\|<origin>` | `RESPONSE\|OK` (internal, node→node) |
| `JOIN\|<node_id>\|<value>\|<origin>\|<host>\|<port>` | `RING_UPDATE\n<count>\n<id>\|<host>\|<port>\n...` |

Try it against a running cluster:

```bash
docker exec node1 sh -c 'printf "PUT|k1|v1|cli" | nc localhost 5001'
docker exec node1 sh -c 'printf "GET|k1|cli"    | nc localhost 5001'
```

## Current limitations (deliberate — this is the Tier 1A work)

- **No durability**: storage is in-memory; a restart loses the node's data.
- **No write acknowledgment**: replication is fire-and-forget, so `OK` only guarantees the coordinator's local write.
- **No versioning / conflict handling**: concurrent writes silently overwrite; there are no vector clocks yet.
- **No read repair or anti-entropy**: replicas that miss a write stay stale until overwritten.
- **Static membership**: nodes are learned at JOIN time only; a dead node is never removed from the ring.
