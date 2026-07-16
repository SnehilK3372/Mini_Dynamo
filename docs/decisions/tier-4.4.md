# Tier 4.4: Horizontal Gateways + Ring-Aware Routing

## What was built

1. **Portable ring hash** (`src/util.h`) — `hash64` reworked from a `std::hash`-seeded value to FNV-1a
   over UTF-8 bytes + the existing MurmurHash3 `fmix64` finalizer, so it's reproducible in Java.
2. **Java hash port** (`gateway/.../cluster/HashUtil.java`) — bit-identical `hash64`, pinned to the
   same golden vectors as `tests/hash_test.cpp` (cross-language contract).
3. **Client-side ring** (`RingRouter.java`) — a `TreeMap<Long, RingNode>` (unsigned-ordered) that
   reproduces `Router::findOwners`; verified against C++-generated owner orderings in `RingRouterTest`.
4. **Ring discovery** (`RingPoller.java` + `@EnableScheduling`) — polls the `RING` command every 5s to
   rebuild the router and refresh the Postgres registry.
5. **Ring-aware routing + Java pool** (`ClusterClient.java`) — routes each keyed op to the key's owners
   (primary first) instead of always the first seed; reuses per-node pooled sockets with half-open
   retry.
6. **Horizontal deployment** — nginx round-robins across 2 gateway replicas (`docker-compose.yml`,
   `deploy/nginx/nginx.conf`); Prometheus discovers replicas via Docker DNS.

## Key design choices

### Replace the hash rather than avoid client-side hashing
**Chose:** make `hash64` portable and hash keys in the gateway.
**Rejected:** keeping `std::hash` and routing blind (first-seed) with only nginx + Java pooling. Blind
routing leaves the real bottleneck — every request coordinates on node1 — untouched; multiple gateways
would just multiply the load onto that one coordinator. Ring-aware routing spreads coordination across
all nodes, which is the actual scalability win. The cost is a one-time ring reshuffle (below).

### FNV-1a + fmix64, seeded on bytes
**Chose:** FNV-1a over raw UTF-8 bytes, then the MurmurHash3 finalizer already in the code.
**Rejected:** full MurmurHash3_x64_128 (more code to port bit-exactly, block + tail handling) and
keeping `std::hash` (unportable). FNV-1a is ~5 lines, trivially identical across C++/Java, and its
distribution — polished by `fmix64` — is more than adequate for a hash ring (not an adversarial
setting). The port's correctness is proven by shared golden vectors in both test suites.

### Routing is an optimization over a correct base, never a dependency
The receiving node always coordinates against **its own** ring, so if the gateway's ring is empty
(before the first poll), stale, or even wrong, the worst outcome is an extra forward hop — never a
wrong answer or a lost write. This is why `RingRouter` starts empty and `ClusterClient` falls back to
the seed list: the gateway serves traffic correctly from the first request and gets *faster* once the
ring is discovered, with no correctness window to manage.

### nginx re-resolves via a proxy_pass variable
**Chose:** `set $upstream http://gateway:8080; proxy_pass $upstream;` with the Docker resolver.
**Rejected:** a static `upstream {}` block. A variable forces per-request DNS resolution, so nginx
round-robins across replicas and picks up scale-up/down without a reload — the compose service name's
A-record returns all replica IPs.

## Where it could break

1. **Ring reshuffle on deploy (breaking).** Changing `hash64` moves every key's position, so a cluster
   mixing old- and new-hash nodes would split key placement (gossip doesn't carry a hash version). All
   nodes must run the same build; deploy all-at-once to a fresh ring. Existing data under the old hash
   becomes unreachable by key — acceptable for this project's redeploy model, but it is a hard cutover.
2. **vnode-count drift.** `RingRouter` uses `cluster.virtual-nodes` (128); if it ever diverges from the
   nodes' `VNODES`, the client ring won't line up and every route degrades to the forward-hop fallback
   (slower, still correct). The two defaults match, and the mismatch is silent — worth a startup assert
   in a later tier.
3. **nginx DNS TTL vs. rapid scaling.** The resolver caches for `valid=10s`; a replica removed within
   that window can still receive a request and 502 until the cache expires. Fine for steady state,
   visible only during fast scale-down.
