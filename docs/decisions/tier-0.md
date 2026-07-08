# Tier 0 — Foundation & Cleanup (decisions log)

## What was built

- Fresh git repository on `main` with a `.gitignore` covering all CMake output; stale artifacts (root `Makefile`, `CMakeFiles/`, `build/`, `kvstore` binary) deleted before the first commit, so no artifact ever enters history.
- Dead code removed: `Node::start()`/`server_loop()` (+ `SEED_HOST`/`SEED_PORT` path), the unused `store`/`store_mtx` pair, FNV `hashKey` (whole `hash.cpp`/`hash.h`), `config/node_config.json`, the nlohmann-json include/dependency — plus, beyond the checklist (flagged in the approved plan): `Node::sendMessage()`, static `tcp_send_recv()`/`read_length_prefixed()`/`split_lines()` in `node.cpp`, dead `split()` in `message.cpp`, and the unused `jsoncpp` Dockerfile packages. All verified to have zero call sites before deletion.
- `StorageEngine` abstract interface (`src/storage/storage_engine.h`) with `put` / `get` / `forEach`; `InMemoryStorageEngine` implements it; `Node` now receives the engine by `unique_ptr` injection and no longer owns a map or a storage mutex.
- Honest `README.md` describing only current behavior, with an explicit limitations section.

## Key design choices (and the rejected alternative for each)

1. **Fresh `git init` instead of a history purge.** The working tree arrived as an unzipped download with no `.git/`. Rejected: reconstructing/cloning an old remote and running `git filter-repo` — there was no local history to purge, and a first commit that is already clean achieves the checklist's intent (no artifacts in history) with zero rewrite risk.
2. **Opaque `string` values in `StorageEngine`.** Tier 1A's `VersionedValue {data, clock}` will serialize into the value. Rejected: templating the interface or defining `VersionedValue` now — it would front-load Tier 1A design decisions into a cleanup tier and force the interface to change twice if the serialization choice shifts.
3. **`forEach` visitor instead of an iterator type.** RocksDB iterators and map iterators share no shape; a callback is the lowest common denominator both implement trivially. Rejected: a cursor/iterator abstraction — more surface, no consumer yet (first real consumer is anti-entropy, which is deferred work).
4. **Thread safety owned by the engine, not the caller.** `TCPServer` spawns a detached thread per connection, so concurrent handler calls are the norm; putting the lock inside the engine makes safety part of the contract instead of caller discipline. Rejected: keeping `Node::storage_mutex` — it couples locking policy to one caller and would not transfer to RocksDB (which is internally synchronized).
5. **Deleting `sendMessage`/`tcp_send_recv` beyond the checklist.** They were reachable only from the already-dead `Node::start()`. Rejected: keeping them "for Tier 1A quorum use" — Tier 1A needs request/response replication with timeouts designed around acks, and resurrecting a half-framed helper would anchor that design; grep confirms nothing live calls them.

## Where this could break under adversarial conditions

- **`forEach` holds the engine mutex for the entire scan.** A slow visitor callback (e.g. a future anti-entropy pass doing network I/O per key) would block all reads/writes on that node for the scan's duration. Acceptable now (no callers); must be revisited before any scan-while-serving feature.
- **Unframed TCP reads are untouched by design this tier**: a request > 4096 bytes, or a slow sender whose bytes arrive in multiple segments, is silently truncated by `TCPServer`'s single `read()`, and a value containing `|` corrupts parsing. Both are known protocol debts; the framing/protocol rework belongs with the Tier 1A wire-format change, not a hygiene tier.

## Verification status (honest)

- All translation units pass `g++ -std=c++17 -fsyntax-only` (POSIX headers stubbed for `node.cpp` on the Windows dev machine).
- **Runtime verification is PENDING**: this machine has no Docker/WSL, so `docker-compose up --build` and the `nc` protocol tests (`tests.txt` scenarios incl. node-kill) have not been run against this commit. User chose to commit now and verify after installing Docker Desktop. Until that passes, Tier 0's definition of done is not fully met.

## Outstanding manual step

- Directory rename `Project_ Mini-Dynamo` → `mini-dynamo` (user will do this manually; it renames the active session's working directory).
