# Tier 4.3: Connection Pooling + Thread Pool + Liveness-Aware Reads

## What was built

1. **Fixed-size server thread pool** (`src/net/thread_pool.{h,cpp}`) — accepted connections are
   dispatched to a bounded worker pool instead of `thread(...).detach()` per connection.
2. **Persistent server connections** (`src/net/tcp_server.{h,cpp}`) — `handleConnection` now loops
   over framed requests on one socket until the peer closes it, instead of closing after one frame.
   This is what makes client-side pooling actually reuse a connection.
3. **Per-peer connection pool** (`src/net/connection_pool.{h,cpp}`) — checkout/checkin/discard of
   reusable sockets, per-peer cap, background idle reaper. Transport-agnostic (injected
   `ConnectFn`/`CloseFn`).
4. **Pooled replica client** (`src/net/pooled_replica_client.{h,cpp}`) — a `ReplicaClient` that runs
   the coordinator's fan-out over pooled connections, with half-open retry and discard-on-error.
5. **Liveness-aware read fan-out** (`src/coordinator.cpp`) — `coordinateGet` skips replicas gossip has
   confirmed dead, mirroring what `writeQuorum` already did.
6. **Pool metrics** — `minidynamo_pool_connections_{created,reused}_total`.
7. **Shared POSIX connector** (`src/net/tcp_connect.{h,cpp}`) — the non-blocking-connect dance
   extracted from `TCPClient` so the one-shot client and the pool dial identically.

## Key design choices

### Fixed thread pool, not epoll/async
**Chose:** a worker pool where each connection occupies a worker for its lifetime.
**Rejected:** an epoll/async reactor. The pool is *connection-bound*, not request-bound — true C10K
would need async I/O. But with client-side pooling capping inbound connections at ~`max_per_peer` ×
(peers + gateways), a node sees a dozen-ish concurrent connections, far under the 64-worker default.
Async I/O is a large rewrite for headroom this tier doesn't need; documented as a known limitation.

### Both sides had to change together
The client pool is useless unless the server keeps the socket open. The server was one-shot
(`handleClient` closed after one frame), so pooling required rewriting it into a per-connection frame
loop. A one-shot client (the Java gateway, unpooled until 4.4) still works: it sends one frame and
closes, which the loop serves as one iteration then an EOF.

### Injected ConnectFn/CloseFn on the pool
**Chose:** the pool holds no POSIX dependency; it takes a dial and a close callback.
**Rejected:** baking sockets into the pool. Injection keeps the pool in `kv_core` (portable, builds on
the Windows dev box) and unit-testable with fake fds — no loopback server, no flakiness — matching the
existing `ReplicaClient`/`StorageEngine` seam philosophy.

### Discard on any send/recv failure; never re-pool a desynced socket
A pooled connection whose response wasn't fully drained (read timeout, partial frame) would corrupt
the *next* caller's framing. So `PooledReplicaClient` only checks a connection back in after a clean
round trip; any failure `discard`s it. The half-open case (peer closed an idle connection we still
hold) is absorbed by retrying once on a fresh connection.

### Liveness-aware reads reuse 4.2's `is_alive_fn`
The chaos-test 18% read errors came from the coordinator firing a read at the dead node on every
request and blocking a thread for the full 500 ms deadline — enough thread/CPU pressure on 2 vCPUs to
push the *live* replica past the deadline too. Skipping known-dead replicas removes the stall. This
reuses the exact `is_alive_fn` already wired for sloppy-quorum writes in 4.2 — no new machinery.

## Where it could break

1. **Worker starvation under many persistent connections.** If concurrent inbound connections ever
   exceed the worker count, new connections queue until a worker frees up. The default (64) is far
   above expected fan-in for a pooled cluster, but a flood of unpooled clients (e.g. many gateway
   instances pre-4.4) could approach it. Mitigation lever: raise `WORKER_THREADS`.
2. **Liveness lag surfacing false `quorum_not_met`.** If gossip is briefly wrong (a node flapping
   Suspect↔Alive), a read could skip a replica that was actually reachable and fail R faster than
   before. This trades a rare fast-fail for eliminating the common-case stall; the write path is
   unaffected (sloppy quorum + hints already handle it).
3. **Stale pooled socket beyond one retry.** The half-open retry is bounded to one fresh attempt. If a
   peer is tearing connections down faster than that (pathological), a read returns empty (treated as a
   non-response) and the quorum logic handles it — correct, but it forfeits that replica for that call.
