# Tier 4.6 — Permanent Node Removal (administrative decommission)

## Why

The testing tier made gossip-detected death stop evicting a node from the ring, which was correct: a
*temporary* failure must not reshuffle key ownership, and evicting on a gossip timeout had silently made
hinted handoff unreachable (Tier 4.2 never stored a single hint). But that fix only implemented one half
of a distinction Dynamo draws deliberately:

- **Temporary failure** — inferred from a timeout. Ring unchanged; the coordinator skips the dead owner
  on reads and hints + stands in on writes. (Done in the testing tier.)
- **Permanent departure** — *asserted* by an operator. The node's ring slots are surrendered.

The second had no path. `EventType::Leave` existed in the enum but nothing emitted it, and `swim.cpp`
treated it as a synonym for `Dead`. So a node that was gone for good kept its vnodes forever: its keys
ran on stand-ins whose hints expired after the 3 h TTL, leaving them under-replicated indefinitely, and
the cluster permanently advertised capacity it did not have. This tier closes that — recorded until now
as constraint §2.0 in `docs/scalability-constraints.md`.

The reason permanent removal has to be a separate, *declared* event is the same reason death cannot
change the ring: no failure detector can tell "slow" from "gone forever". Only a human knows a node is
never coming back.

## What was built

- **`MemberState::Left`** — a fourth SWIM state, distinct from `Dead`: removed from the ring, and a
  *terminal, sticky tombstone*.
- **`Swim::leave()` / `hasLeft()` / `stateOf()`**, and a dedicated `EventType::Leave` case in
  `applyEvent()` that is **not** gated on incarnation.
- **`GossipThread::requestLeave()`** + the ring-eviction half of the `onMemberChange` callback (the
  first-ever caller of `Router::removePhysicalNode`), and a `tick()` that stops probing once retired.
- **`LEAVE|<node_id>`** wire verb (`Node::handleLeave`), sendable to *any* live node.
- **`HintStore::dropTarget()`**, wired in `main.cpp` to the `Left` callback so a departed node's
  undeliverable hints are discarded instead of lingering for the TTL.
- Tests: 14 new `SwimTest` cases (the tombstone rules) and 5 new `Cluster` scenarios (multi-node
  propagation, the no-rejoin regression, ownership reshuffle, self-retirement, hint drop). Harness gains
  `decommission()` / `waitForLeftEverywhere()`.

## Key design choices

### `Left` is a tombstone, not just "removed" — and the tombstone is the load-bearing part
**Chose:** a terminal state every revival path checks, rather than simply deleting the member record.
**Why it must be a tombstone, not a delete:** `applyDirectJoin()` is deliberately *not* gated on
incarnation — that is the Tier-testing rejoin fix, because a restarted process legitimately returns at
incarnation 0 and must be believed. With no incarnation guard on that path, **the tombstone is the only
thing between a decommissioned node and a walk straight back into the ring** the moment its box reboots.
A deleted record would be recreated by the very next `Join` or `Alive` the node sends. So `Left` is
checked in four places: `applyDirectJoin` (the handshake), the `Join`/`Alive` apply path (relayed
gossip, for a node still running and refuting), and both `Suspect` and `Dead` (so nothing demotes it to
a revivable state). `confirmDead()` refuses to demote it too.

**Rejected:** reusing `Dead`. Dead has to stay revivable (that is the whole point of the last tier), so
it cannot double as "never comes back". The two are genuinely different events and now have genuinely
different states — `TemporaryFailureDoesNotReshuffleOwnership` and `PermanentRemovalReshufflesOwnership`
are the same setup with opposite assertions, pinning exactly that.

### Leave is unconditional on incarnation
**Chose:** a Leave wins against a member held at *any* incarnation, and `applyEvent`'s Leave case has no
`event.incarnation` comparison at all.
**Rejected:** gating it like every other event. An operator's decision is not a stale observation to be
outvoted by a newer incarnation — and gating would make decommission fail precisely against a *flapping*
node whose incarnation is racing ahead, which is the node you most want to retire.
`LeaveWinsRegardlessOfIncarnation` pins this.

### A refused joiner is told, not stonewalled
**Chose:** `applyDirectJoin` returns `std::optional<uint64_t>` — `nullopt` means "refused, disseminate
nothing". The SWIM_JOIN handler answers a refused (tombstoned) joiner with an ack carrying a single
`Leave` naming it, which trips the joiner's own self-Leave branch so it retires itself.
**Why `nullopt` and not just a bool:** the pre-existing caller unconditionally broadcast `Alive` at the
returned incarnation. An `Alive` on behalf of a departed node would be *rejected* by every peer holding
the tombstone but *accepted* by any that had not yet heard — quietly reviving it in one corner of the
cluster. The type change forces the caller to disseminate nothing.
**Why tell it at all:** otherwise the retired process retries forever, believing it is a member and
serving reads from a ring nobody agrees with. `ARefusedJoinerRetiresItself` covers this end to end.

### Self-Leave is obeyed, not refuted
**Chose:** a Leave naming *self* sets `self_left_`, fires the callback (so the node drops itself from its
own ring) and is passed on once — but never refuted.
**Rejected:** letting it fall through the self branch. Suspect/Dead about self are inferences a healthy
node is *entitled* to refute by bumping its incarnation; a Leave is a decision, and a node that refuted
its own retirement could never be decommissioned while it was still running. `LeaveAboutSelfIsNotRefuted`
and `SelfSuspicionIsStillRefutedAfterLeaveHandling` pin both halves.

### Tombstone an unknown node too
**Chose:** a Leave for a node we have never heard of still records a `Left` entry.
**Why:** a Leave can overtake the `Alive` it refers to (both race through the same dissemination
stream). Dropping it would leave no tombstone, and the trailing `Alive` would add a departed node to our
ring by itself. `LeaveForAnUnknownNodeStillTombstones` pins it.

### The LEAVE verb lives on the node port, not the gateway
**Chose:** a raw `LEAVE|<id>` verb on the node's internal TCP port, actionable from any live node about
any node.
**Rejected:** a JWT-guarded gateway REST endpoint. It roughly doubles the tier (a Java admin path, a
role check, JUnit) and raises a genuinely new authorization question, for a demo-scale operator action.
The internal port already accepts `REPLICATE` (arbitrary writes), so it was never a security boundary —
adding LEAVE there introduces no new trust boundary, only a larger blast radius, which the constraints
doc states plainly.

## Verification

- **137 → tier total green** in CI's `debian:bookworm-slim`, `clang-format-14` clean, node binary links.
- **Mutation-proved the regression tests fail against the bug they target** — the check that matters
  most. Reintroducing each guarded bug makes the corresponding test fail; restoring makes it pass:
  removing *only* the `applyDirectJoin` tombstone line breaks `LeaveIsStickyAgainstDirectJoin` and
  `DecommissionedNodeCannotRejoin`; making `Leave` a synonym for `Dead` breaks the removal/ownership
  tests; dropping the ring eviction breaks propagation; removing the relayed-Alive check breaks
  stickiness; un-special-casing self-Leave breaks self-retirement.
  - *Process note:* the first mutation harness ran on the OneDrive/Docker bind mount, where `make`'s
    mtime tracking is unreliable — it reused stale objects and mis-scored two live mutations as "test
    passed". Rerun on the container's native filesystem with a clean build, the scores are honest. A
    mutation harness that lies is worse than none; the fix was to move it off the bind mount, not to
    trust the green.

## Where this could still break (adversarial conditions)

1. **Decommission fixes ownership, not data placement.** Removal reshuffles the ring so the *right*
   nodes own the departed node's keys — but they hold nothing for those keys until something populates
   them. Read repair heals only keys that are read; hinted handoff heals only the outage window. The
   general fix is anti-entropy, which is **still stubbed** (constraints §2.1). So after a decommission,
   unread keys stay under-replicated until read. This is strictly better than the pre-4.6 state
   (permanently-wrong ownership), not good.
2. **Tombstones are in-memory.** A decommission is forgotten only if *every* node restarts at once —
   narrow, because a permanently-dead node never returns, and a fresh node learns membership from peers
   that exclude `Left`, so it never hears of the departed one. The real exposure is decommissioning a
   node that is still running and then restarting the whole cluster. Persisting tombstones was
   considered and deferred; it wants a metadata column family and a GC story that a real cluster-config
   store should own.
3. **Do not decommission the live seed.** If the target is still running *and* is the seed, new joiners
   would learn a ring containing it (it answers their JOIN) while existing nodes hold tombstones — a
   split view. Retire the seed only when it is actually down, or after repointing `SEED_NODES`.
4. **Larger blast radius on the internal port.** Any client that can reach a node's internal port can
   now retire any node. That port already accepted arbitrary `REPLICATE` writes, so this is not a new
   trust boundary — but it is a more destructive verb on an unauthenticated surface.
