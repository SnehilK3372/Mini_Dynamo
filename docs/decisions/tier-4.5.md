# Tier 4.5: Multi-Host Deploy + Vector-Clock Pruning + Scale Benchmark

## What was built

1. **Vector clocks carry timestamps and are bounded** (`src/vector_clock.{h,cpp}`) — each entry is now
   `{counter, updated_ms}`; `prune(max)` drops the oldest entries. Wire form changed from
   `node:counter` to `node:counter:ts` (**breaking**).
2. **Pruning wired in** (`src/coordinator.cpp`, `src/main.cpp`) — `bumpedClock()` prunes after the bump;
   bound via `QuorumConfig::max_clock_entries` / `MAX_CLOCK_ENTRIES` (default 20).
3. **Multi-host Swarm stack** (`deploy/swarm/`) — one scalable `kvstore` service + a bootstrap `seed`
   service on an overlay network, with `init-swarm.sh` and `deploy.sh`.
4. **Scale benchmark** (`bench/scale/`) — uniform 70/30 workload, a 5→100 driver that waits for gossip
   convergence before measuring, and a RESULTS template.
5. **Multi-host smoke test** (`tests/multi_host_smoke.sh`) — asserts the ring really spans hosts, then
   drains a host and checks availability + recovery.

## Key design choices

### Timestamps travel on the wire (accepting a breaking format change)
**Chose:** serialize `node:counter:ts`.
**Rejected:** local-only timestamps (no format change). If timestamps don't replicate, every node
assigns its own notion of "recent", so different replicas prune *different* entries from the same
logical clock and manufacture avoidable conflicts. Shipping the timestamp makes pruning deterministic
cluster-wide. The cost is a breaking wire/storage change — acceptable here because Tier 4.4 already
forces a fresh cluster (ring reshuffle), so both land in one cutover. `parse()` still accepts the
legacy 2-field form (ts=0 → oldest) so a stray old value degrades instead of throwing.

### Timestamps are pruning metadata, never causality
`compare()` and `operator==` read **counters only**. A clock that round-trips through a peer keeps its
history regardless of stamps, and a fresh timestamp can never make a stale counter look ahead. This is
the invariant that keeps the format change from touching correctness — verified directly in
`vector_clock_test.cpp`.

### Prune after the bump, and break ties by node id
Pruning *after* `set(self, base+1)` means our own entry is the newest and can never be the one dropped
— the write being coordinated always survives in the clock it carries. Ties on `updated_ms` are broken
by node id so two nodes handed an identical clock produce an identical result; without that, pruning
itself would cause replicas to diverge.

### A dedicated `seed` service instead of `SEED_NODES=tasks.kvstore:5001`
**Chose:** a 1-replica `seed` service that self-bootstraps; `kvstore` replicas seed off `tasks.seed`.
**Rejected:** every replica seeding off its own service. `tasks.<service>` resolves to **all** task IPs
*including the caller's own*, so a replica can seed off itself, learn nothing, and stay invisible
forever (gossip only probes peers it already knows). At ~1/N odds per replica that is near-certain by
100 nodes. Seeding off a *different* service makes self-resolution impossible. The seed is special only
at join time — SWIM membership is peer-to-peer afterwards, and the seed is an ordinary ring member
(so ring size = replicas + 1).

### Registry as a Swarm-published service
`docker stack deploy` ignores `build:`, so every host must *pull* the image. Publishing `registry:2` on
port 5000 makes it reachable at `127.0.0.1:5000` from every host via the routing mesh, and Docker
treats `127.0.0.1:5000` as insecure-by-default — no TLS setup. This also enforces the "all nodes run
one build" rule that both the 4.4 ring hash and the 4.5 clock format depend on.

## Where it could break

1. **Pruning is not strictly loss-free — and the original plan overclaimed this.** The realistic
   outcome is conservative: a pruned clock compares CONCURRENT and surfaces siblings. But a *false
   dominance* is possible in principle: if `B={x:5}` were pruned to `B'={}` while `A={x:1}` keeps `x`,
   `A` would appear to supersede `B` and could overwrite it. It requires the pruned clock to shed
   **every** entry carrying its dominance, which `max_clock_entries=20` puts out of practical reach (a
   pruned clock still retains 20 entries, and the pruned ones are the least-recently-touched). The
   Dynamo paper makes and documents the same trade — truncation "can lead to inefficiencies… as the
   descendant relationships cannot be derived accurately". Lowering `MAX_CLOCK_ENTRIES` aggressively
   moves this from theoretical toward real.
2. **Wall-clock skew between hosts.** `updated_ms` uses `system_clock` because the stamps must share an
   epoch across nodes. Skew only perturbs *which* entry looks oldest — it cannot affect causality — but
   a badly skewed host could bias pruning toward the wrong entries. NTP is assumed.
3. **Swarm env templating.** Identity depends on `{{.Task.Slot}}`/`{{.Task.Name}}` expanding and task
   names resolving on the overlay. If a Swarm version doesn't, nodes get duplicate ids or unreachable
   advertise hosts; the fallback is an entrypoint deriving `NODE_ID` from the container hostname.
4. **The seed is a join-time single point.** If it is down, *new* nodes cannot join (existing membership
   is unaffected, since SWIM is peer-to-peer). Fine for a benchmark; a production system would run
   several seeds.
