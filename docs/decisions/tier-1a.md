# Tier 1A — Complete the Distributed Core (decisions log)

## What was built

The four dependency-ordered pieces that make the "Dynamo" claim true:

1. **Durable per-node storage.** `RocksDBStorageEngine` (`src/storage/rocksdb_storage.cpp`)
   implements the Tier-0 `StorageEngine` interface over an embedded RocksDB instance in the
   node's own directory (`DATA_DIR`, default `/data/<node_id>`) — shared-nothing. Selected at
   startup by `STORAGE_ENGINE` (`rocksdb` default when compiled with RocksDB, `memory`
   otherwise/for tests). The engine stays a dumb byte store; a serialized `VersionedValue`
   is what it holds.

2. **Tunable quorum (N/W/R).** `Coordinator` (`src/coordinator.cpp`) replaces fire-and-forget
   replication. A write fans out to the N owners and returns `OK` only once **W** acknowledge
   within a deadline (else `quorum_not_met`, a retryable status); a read gathers **R** of N
   responses before answering. `REPLICATE` is now request/response (`RESPONSE|OK`), and a new
   internal `READ` fetches a replica's `VersionedValue`. Defaults `N=3, W=2, R=2`, tunable
   per request.

3. **Vector clocks.** `VectorClock` (`src/vector_clock.cpp`) with `compare()` →
   `EQUAL / A_DOMINATES / B_DOMINATES / CONCURRENT`, and `VersionedValue {data, clock}`
   (`src/versioned_value.cpp`). On PUT the coordinator bumps its own entry above
   `max(client-context, local-stored)`; on GET it reduces the responses to the maximal
   (non-dominated) set — one winner, or concurrent **siblings** returned to the client.

4. **Read repair.** After a single-dominant GET, the coordinator asynchronously pushes the
   winner to every strictly-stale or missing replica, incrementing a `read_repair_count`
   behind a `Metrics` interface (`src/metrics.h`). It never blocks the read.

Supporting change: **length-prefixed framing** (`src/net/framing.cpp`) and **base64-encoded
values**, so the richer payloads (clocks, multi-sibling responses, arbitrary bytes) travel
reliably. Coordination logic is decoupled from sockets behind `ReplicaClient`
(`src/replica_client.h`; TCP impl in `src/net/tcp_replica_client.cpp`), which is what makes
the core unit-testable without a network.

## Key design choices (and the rejected alternative for each)

1. **Quorum, not consensus.** Leaderless N/W/R keeps the system available under partition
   (in the limit, `W=1` accepts a write if any one replica is up) and pushes the cost onto
   eventual convergence. *Rejected:* Raft/Paxos — linearizable but CP; the minority side
   stops serving during a partition, sacrificing the availability that is Mini Dynamo's whole
   reason to exist. Choosing quorum *is* choosing AP over CP.

2. **Vector clocks, not last-write-wins.** LWW-by-timestamp relies on skewed wall clocks and,
   worse, *silently discards* one of two concurrent writes — it can't even tell a conflict
   happened. Vector clocks detect causality vs. concurrency and preserve conflicts as
   siblings. *Rejected:* LWW (data loss) and CRDTs (constrain the value to specific mergeable
   types; wrong for an opaque KV store).

3. **Read repair first; hinted handoff and anti-entropy named as future work.** Read repair
   is the cheapest convergence mechanism — it rides the existing read path and needs no
   background machinery. But it only heals keys that are *read*. **Hinted handoff** (write to
   a stand-in when an owner is down, deliver on recovery → always-writeable) and **Merkle-tree
   anti-entropy** (periodic background repair of rarely-read keys) complete the story and are
   deferred to Tier 3. *Rejected:* building all three now — scope creep against a tier whose
   job is to land the core correctly.

4. **Coordinator bumps above `max(context, local)`.** Guarantees a new write dominates both
   the client's context and the coordinator's own copy. *Rejected:* naive `context[coord]+1`
   — a blind write (empty context) would produce a clock dominated by the stored version and
   be silently lost on the next read.

5. **Length-prefixed framing + base64 values, keeping the pipe format.** The old single
   ≤4096-byte `read()` truncates on TCP segmentation or large responses — survivable for bare
   strings, not for clocks + siblings. Framing fixes it; base64 makes arbitrary value bytes
   safe without an escaping scheme, and the control tokens/clock stay delimiter-free so the
   parser stays a simple split. *Rejected:* re-introducing JSON (Tier 0 removed nlohmann-json
   on purpose) or a full netstring rewrite (larger change, no added payoff here).

6. **Coordination decoupled behind `ReplicaClient`; background threads drained in the
   coordinator's destructor.** The quorum fan-out uses detached threads over a shared
   `condition_variable` state so a slow replica's late ack is simply ignored past the deadline
   — *not* `std::async`, whose abandoned-future destructor would *join* the slow task and hang
   the coordinator. The `Coordinator` destructor then waits for all outstanding background
   threads, so no detached worker can outlive the coordinator or the dependencies it borrows.
   *Rejected:* `std::async` (hangs on slow peers) and letting detached workers capture `this`
   (a use-after-free at teardown — caught as a real segfault during testing and fixed).

## Where this could break under adversarial conditions

- **GET-consistency asymmetry (expected AP behavior, not a bug).** Read repair is
  asynchronous, and a read only consults R of N replicas. During the repair window, two
  clients that read different R-subsets can observe different answers **even at `W+R>N`** —
  one may hit a not-yet-repaired stale replica. `W+R>N` guarantees the read set overlaps the
  last *completed* write set, but not that every replica is current. This is eventual
  consistency working as designed; strict per-read agreement would require CP machinery.

- **A single replica holds one version.** On `REPLICATE`, a replica stores the incoming
  version unless its local copy strictly dominates (never regress). Concurrent versions are
  surfaced only because *different* replicas hold different ones and the read reconciles
  across them; two concurrent writes landing on the *same* replica sequentially collapse to
  one there. Full per-replica sibling storage is deferred with anti-entropy.

- **Coordinator/ReplicaClient lifetime contract.** Background workers assume the coordinator
  and the replica client outlive them; production satisfies this (the `Node` owns both for
  the whole process), and the destructor drain enforces it at shutdown/in tests. A future
  refactor that tears these down mid-flight would need to preserve that ordering.

## Verification status (honest)

Verified on two toolchains: the Windows/MSYS2 `ucrt64` dev box (pure-logic suite) and a
WSL **Ubuntu 24.04** environment with real RocksDB 8.9 and POSIX sockets (everything else).

- **Unit suite — 30/30 green.** vector-clock compare, versioned-value/base64 round-trips, the
  full `Coordinator` suite (W met / not met / **slow-ack-past-deadline doesn't count and
  doesn't hang** / R met / downed replica / **dominance** / **concurrency → siblings** /
  **read repair** synchronized via the fake's `waitForWrites`), router placement, and the 3
  **RocksDBStorageEngine** tests (survive-reopen, serialized `VersionedValue`, shared-nothing
  independence) — the last run for real against librocksdb in WSL.
- **Node binary builds and links for real.** `kvstore` compiles and links with real POSIX
  headers + RocksDB in WSL (g++ 13.3), so the whole socket path (`framing`, `tcp_client`
  connect/read timeouts, `tcp_server`, `tcp_replica_client`, `node`, `main`) is verified, not
  just syntax-checked.
- **End-to-end scenario — 8/8, twice.** First on a native 3-process loopback cluster (peers
  resolved via `HOSTALIASES`), then on the **real `docker-compose` cluster** (`node1/2/3`,
  published ports, per-node RocksDB). Both proved the whole thesis: `PUT W=2` acknowledges;
  with a replica killed (`docker stop node2`) the write and the read **stay available**; the
  killed node's RocksDB **persists its value across the restart** (`docker start node2`); and
  a subsequent read **repairs the stale replica to convergence**. This confirmed that the
  *ring* (not the receiving node) selects the per-key coordinator, so clocks are stamped by
  the key's primary owner.
- **Canonical CMake build — verified.** `docker compose up --build` builds each node via the
  Dockerfile's `cmake -S . -B build && cmake --build build` on `debian:bookworm-slim` with
  `librocksdb-dev`, so the CMakeLists + RocksDB detection path is exercised end to end.
- **Nothing deferred.** Every piece of Tier 1A — durable RocksDB, quorum, vector clocks, read
  repair, framing, the socket path — has been compiled, linked, and run. No behavior is
  claimed that wasn't executed.
