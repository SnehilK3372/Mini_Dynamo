# Testing Tier — In-Process Cluster Harness + Vector-Clock Properties

## Why

The pre-deploy audit found a bug **89 passing tests missed**: a restarted node could never rejoin, which
also left hinted handoff inert (it fires on `Dead → Alive`). The test that should have caught it,
`FreshJoinRevivesDead`, used an artificially higher incarnation — it encoded my assumption instead of a
real restart.

A coverage map showed why: the **pure value types were well tested; the threads, protocol drivers and
wiring were not** (`handoff_thread`, `gossip_thread`, `node`, `antientropy_thread` had zero tests) —
exactly inverted from where the risk lives. Structurally there was a chasm: unit tests stub the cluster
away with a fake, e2e needs Docker and covers one scenario, and **nothing tested several real nodes
talking to each other**.

## What was built

1. **`tests/support/in_process_cluster.h`** — N real nodes in one process: real `Coordinator`,
   `GossipThread`/`Swim`, `HintStore`/`HandoffThread`, `Router`. Only the *transport* is swapped, via
   the seams that already existed (`ReplicaClient`, `SendFn`, `DeliverFn`). `kill()`/`restart()` model
   a crash and a genuine process restart (**fresh Swim → incarnation 0**).
2. **`tests/cluster_test.cpp`** — 9 scenarios: convergence, failure detection, ring stability under
   transient failure, **restart/rejoin**, **hint delivery on recovery** (`HandoffThread`'s first test
   ever), read repair, siblings, quorum failure.
3. **`tests/vector_clock_property_test.cpp`** — 11 randomised properties over the correctness core.
4. **`src/replica_ops.{h,cpp}`** — the never-regress replicate rule, extracted from `node.cpp` so the
   harness runs the *shipped* code rather than a copy of it.
5. **Build**: `log.cpp` + `gossip_thread.cpp` moved into `kv_core` (both portable), which is what makes
   `GossipThread` linkable by tests at all.

Whole suite: **114 tests, ~4 seconds**, no Docker.

## Bugs the harness found on its first run

All three were live in `main`, all shipped through a green suite.

1. **Hinted handoff was unreachable — Tier 4.2 had never stored a single hint.** A node is `isAlive`
   while `Alive` *or* `Suspect`, and the instant it turned `Dead` gossip removed it from the ring — so
   it was never an owner the coordinator could see as dead, and `hint_store_->store()` could not be
   reached. The live cluster's `hints_stored_total` was 0, and I had read that as "no outage yet"
   rather than "impossible".
2. **A same-incarnation relay cleared suspicion, forever.** `applyEvent` accepted `>=` for a Suspect
   node, and `applyIncomingEvents` re-enqueues anything that changed state — so a stale relayed
   `Join`/`Alive` flipped Suspect→Alive, got re-disseminated, came back, and flipped it again. **A dead
   node could never be declared dead.** This is why cluster tests took 30s before the fix and 0.15s
   after.
3. **A fabricated ack.** With no stand-in available, `writeQuorum` wrote locally and incremented
   `localAcks` *again* — but if `self` was already an owner it had already stored and acked. One
   physical copy satisfied two acks, so `W=3` could "succeed" against two replicas.

## Key design choices

### Dead nodes stay in the ring (the fix for #1)
**Chose:** gossip-detected death marks liveness in Swim but leaves the ring untouched; the coordinator
consults `is_alive_fn` per request (skip on reads — already there from 4.3 — hint + stand-in on writes).
**Rejected:** evicting on death (the old behaviour). It conflated Dynamo's two distinct events —
*temporary failure* (ring unchanged; sloppy quorum + hints) and *permanent departure* (an explicit
administrative ring change) — and cost twice: every transient blip reshuffled key ownership, and hinted
handoff became dead code. `TemporaryFailureDoesNotReshuffleOwnership` now pins the property.

*Consequence to accept:* a permanently-dead node keeps its ring slots forever, so its keys run on
stand-ins whose hints expire after the TTL. Permanent removal needs an administrative path (`Leave`
exists in the enum but nothing emits it) — recorded in `docs/scalability-constraints.md`.

### A direct join is authoritative; relayed gossip is not
**Chose:** split `applyDirectJoin()` (the SWIM_JOIN handshake — the node speaking for itself, accepted
whatever incarnation it carries) from `applyEvent()` (relayed third-party gossip — strictly-newer
incarnation only).
**Rejected:** my first fix, which special-cased `Join` inside `applyEvent`. It could not tell a
handshake from a *relayed* Join event, so a Join circulating in the dissemination stream would have
resurrected genuinely dead nodes forever — trading one bug for a worse one.

The reviver also **bumps past its own stale record** and disseminates `Alive` at that incarnation.
Re-broadcasting the joiner's own (possibly 0) incarnation would be rejected by every peer still holding
`Dead@0`: the node would rejoin for the seed and stay invisible to everyone else.

### Hand-rolled generators, not RapidCheck
**Chose:** ~40 lines of generator with a fixed seed, printing the seed and counterexample on failure.
**Rejected:** RapidCheck — a new dependency against a project that deliberately keeps them minimal
(Tier 0 removed nlohmann-json). What's given up is automatic shrinking; clocks this small print
readably as-is. Flagged rather than silently deviating from the stack.

### Properties earn their keep on the clock
`PruningNeverFlipsTheWinnerAtTheProductionBound` turns the prose caveat in `tier-4.5.md` into an
executable probe: it generates pairs where `a` genuinely dominates `b`, prunes both at
`MAX_CLOCK_ENTRIES=20`, and asserts the verdict never flips to a wrong winner (silent data loss), while
*recording* how often it degrades to CONCURRENT — the documented, acceptable outcome.

## Verification

- **114/114 green, three consecutive runs, ~4s** in CI's `debian:bookworm-slim`. No flakes.
- **The regression tests were proven to fail against the bug they target.** Re-introducing ring
  eviction makes `HintIsDeliveredOnRecovery` and `TemporaryFailureDoesNotReshuffleOwnership` fail
  (exit 1); restoring makes them pass. A regression test that never fails against its own bug is
  theatre, so this check matters more than the green run.
- The node binary still builds and links after the `kv_core` move and the `replica_ops` extraction.

## Where this could still break

1. **Timing.** The harness runs real gossip threads at a 20ms period. It is stable over repeated runs,
   but it is wall-clock dependent; a heavily loaded CI box could in principle need longer ceilings. The
   fix is a longer timeout, never a `sleep`.
2. **The harness is not the full node.** It wires `Coordinator` + gossip + handoff, not `Node`'s
   protocol edge — `node.cpp` needs POSIX framing/sockets, so parsing and forward-to-primary remain
   untested. `replica_ops` keeps the replicate path honest; testing `Node` wants `handleRequest` to
   return a string instead of writing to an fd.
3. **Properties are only as good as their generators.** The clock generators use a small alphabet and
   small counters so dominance actually occurs; a wider range would make nearly every pair CONCURRENT
   and quietly test nothing. `DominanceIsTransitive` asserts its own sample is non-vacuous for this
   reason.
