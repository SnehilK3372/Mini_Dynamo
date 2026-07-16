# Tier 4.4 — Horizontal Gateways + Ring-Aware Routing

## Context

The gateway is a single throughput ceiling and, worse, a *coordination funnel*:
`ClusterClient.exchange` (`gateway/.../cluster/ClusterClient.java:139`) tries `cluster.nodes` in
order and only fails over on a connection error, so every request lands on the first node (node1),
which then coordinates the quorum. Multiple gateways alone wouldn't fix this — they'd all still hammer
node1. The Tier-4.3 chaos run showed exactly this hotspot.

Tier 4.4 makes the gateway horizontally scalable **and** ring-aware: several stateless gateway
instances behind nginx, each hashing the key locally and routing straight to the key's primary owner —
spreading coordination across all nodes and removing the forward hop.

**Blocker resolved:** ring-aware routing requires the gateway to reproduce the cluster's ring hash, but
`hash64` (`src/util.h:8`) seeds off `std::hash<std::string>`, which is implementation-defined and
unreproducible in Java. Fix: replace the seed with a fully-specified byte hash (FNV-1a over UTF-8 +
the existing MurmurHash3 `fmix64` finalizer already in `hash64`). This is portable and bit-identical
across C++/Java. **Safety property:** a Java/C++ ring mismatch can only cost an extra forward hop —
never correctness — because whichever node receives a request coordinates against *its own* ring
(`Node::handlePut` forwards to its own primary; reads coordinate locally). Routing is a pure
optimization layered on a correct base.

Outcome: throughput scales with gateway count; coordination spreads across nodes; the node1 funnel is
gone. Caveat: changing `hash64` reshuffles the ring → deploy to a **fresh cluster** (all nodes same
build; documented).

## Approach

### A. Portable ring hash (C++)
Rewrite `hash64` in `src/util.h` to drop `std::hash`: FNV-1a 64-bit over the string's UTF-8 bytes
(offset basis `1469598103934665603`, prime `1099511628211`), then the *unchanged* `fmix64` finalizer
already present. No other C++ change — `Router::insertVNode`/`findOwners` (`src/router.cpp:15,50`) call
`hash64` unchanged. Add `tests/hash_test.cpp` asserting golden vectors for a fixed set of keys (these
become the cross-language contract).

### B. Java hash port + cross-language test
New `gateway/.../cluster/HashUtil.java` — `static long hash64(String)` implementing the identical
FNV-1a + `fmix64`, using UTF-8 bytes, `>>>` (unsigned shift), and 64-bit `long` wraparound.
`HashUtilTest.java` asserts the **same golden vectors** as `tests/hash_test.cpp` — this is the proof
the two rings agree.

### C. Java client-side ring (`RingRouter`)
New `gateway/.../cluster/RingRouter.java`: a `TreeMap<Long, RingNode>` keyed with a
`Long::compareUnsigned` comparator (to match C++ `std::map<uint64_t>` ascending order). Built from the
current node list by inserting, for each node and each `i` in `[0, virtualNodes)`, the vnode at
`HashUtil.hash64(nodeId + "#vn" + i)` — mirroring `Router::insertVNode` label format exactly.
`ownersFor(key, n)` walks clockwise (`ceilingEntry`→wrap to `firstEntry`) collecting `n` distinct
physical owners, matching `Router::findOwners` semantics. `virtualNodes` must equal the C++ default
(128) or routing degrades to the forward-hop fallback.

### D. Ring discovery (`RingPoller`)
New `gateway/.../cluster/RingPoller.java`: `@Scheduled(fixedDelayString="${cluster.ring-poll-interval-ms:5000}")`
calls the existing `ClusterClient.ring()` (the read-only `RING` command), rebuilds `RingRouter`'s node
set, and upserts the Postgres registry via the existing `NodeRegistryService`. Seed from
`cluster.nodes` at startup. Add `@EnableScheduling` to `GatewayApplication`.

### E. Ring-aware routing + Java connection pool (`ClusterClient`)
- `put/get/delete` already receive the key: compute a preferred target order via
  `RingRouter.ownersFor(key, n)` and pass it to `exchange`, which tries the primary first, then the
  other owners, then any remaining node as last-resort failover (preserving today's availability). The
  `ring()` call (no key) uses any node.
- Replace per-request `new Socket()` with a small per-node pool (`Map<String, Deque<Socket>>` +
  checkout/checkin/cap/health) — the Java analogue of Tier 4.3's `ConnectionPool`. Discard on any
  IO error; retry once on a fresh socket (half-open handling), mirroring `PooledReplicaClient`.

### F. Deployment: nginx + scaled gateways
- New `deploy/nginx/nginx.conf`: round-robin `upstream` to the gateway replicas via the Docker
  resolver (`resolver 127.0.0.11; set $up http://gateway:8080; proxy_pass $up;`) so both replicas are
  picked up.
- `docker-compose.yml`: add an `nginx` service publishing `8080`, proxying to `gateway`; scale
  `gateway` to 2 replicas (drop its `container_name` and host port publish so it can scale; nginx
  fronts it). Update `deploy/prometheus/prometheus.yml` to scrape gateway replicas via
  `dns_sd_configs` (Docker DNS) instead of a single `gateway:8080` target.

### G. Config
`ClusterProperties`: add `ringPollIntervalMs` (5000), `virtualNodes` (128), `maxConnectionsPerNode`
(4). Wire the same into `application.yml`/compose env.

## Files

- **New:** `gateway/.../cluster/HashUtil.java`, `RingRouter.java`, `RingPoller.java`;
  `gateway/.../cluster/NodeConnectionPool.java` (or fold pooling into `ClusterClient`);
  `gateway/src/test/java/.../cluster/HashUtilTest.java`, `RingRouterTest.java`;
  `tests/hash_test.cpp`; `deploy/nginx/nginx.conf`; `docs/decisions/tier-4.4.md`
- **Modified:** `src/util.h` (portable hash); `tests/CMakeLists.txt` (+hash_test);
  `gateway/.../cluster/ClusterClient.java` (ring-aware target order + pooling);
  `gateway/.../config/ClusterProperties.java` (+3 props); `GatewayApplication.java`
  (`@EnableScheduling`); `gateway/.../service/KvService.java` only if the target must be threaded
  through (likely unchanged — routing stays inside `ClusterClient`); `docker-compose.yml`;
  `deploy/prometheus/prometheus.yml`; `gateway/src/main/resources/application.yml`

## Tests

- **`tests/hash_test.cpp`** + **`HashUtilTest.java`**: identical golden `hash64` values for a fixed key
  set — the cross-language contract.
- **`RingRouterTest.java`**: a fixed 3-node set routes known keys to the expected primary and returns N
  distinct physical owners in ring order (compare against values derived from the C++ golden vectors).
- **Integration (compose-based, scripted):** 2 gateways + nginx + 3 nodes — both gateways serve
  traffic; killing/adding a node is reflected in routing within one poll interval; existing
  `scripts/e2e.sh` still passes through nginx.

## Verification

1. **Cross-language hash**: `ctest` (hash_test) and `./mvnw test` (HashUtilTest) both green on the same
   vectors — proves the rings agree.
2. **Routing hits the primary**: with the stack up, a PUT/GET through the gateway reaches the key's
   primary directly (no forward) — confirm via per-node `minidynamo_requests_total` showing
   coordination spread across nodes, not concentrated on node1.
3. **Horizontal scaling**: `bench/run.sh` through nginx with `gateway` scaled 1→2 shows throughput
   rising toward ~2× and p99 holding; `scripts/e2e.sh` passes through nginx unchanged.
4. **Discovery**: add a 4th node (gossip) → within `ringPollIntervalMs` the gateway routes keys to it.

## Sequencing & risk
- Order: (A) hash → (B) Java hash + cross-test → (C) RingRouter → (D) poller → (E) routing+pool →
  (F) nginx/compose. Each step is independently testable; the tier can be split at the nginx boundary
  (E vs F) if a smaller PR is wanted.
- **Ring reshuffle** (breaking): all nodes must run the same build; a mixed old/new hash cluster splits
  key placement. Deploy all-at-once to a fresh cluster. Documented in the decisions log.
- **Java/C++ ring drift**: only ever costs a forward hop, never correctness (receiving node coordinates
  against its own ring). Makes routing a safe, best-effort optimization.
