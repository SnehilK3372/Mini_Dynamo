# Tier 4.7 — Membership Anti-Entropy + Audit Hardening

## Why

The post-4.6 project-wide audit found 11 issues. This tier fixes the six selected: two cheap
correctness holes (the legacy `JOIN` verb, unenforced node-id charset), the observability gap (no way
to see ring divergence), two doc inconsistencies (runbook silent on LEAVE, a false frame-cap
comment), and the one structural scale blocker: **membership had no anti-entropy**.

The structural problem: gossip events piggyback with a finite dissemination budget
(`3·⌈log₂N⌉` sends), and the only full membership exchange in the whole system was the one-shot
SWIM_JOIN ack. Any event a node missed — partition, drop, timing — was missed **forever**:

- A node partitioned through a `Leave` held the departed node in its ring permanently. The operator's
  decommission silently never took there — divergent replica sets, hints accumulating for a corpse.
- Worse than the audit stated: a node partitioned past the suspicion timeout was marked Dead by
  everyone, and healing the partition could **never** bring it back. Peers don't probe Dead nodes,
  its own pings revive nothing, `joinViaSeeds` runs only at boot, and the refutation events were
  budget-exhausted. A healthy, running node stayed invisible until a process restart — the
  Tier-testing rejoin bug's sibling, for partitions. `HealedPartitionRecoversWithoutRestart` was
  demonstrated failing against pre-4.7 code.

The probability that *someone* misses an event grows with N and churn: a works-at-5, rots-at-100
defect, which is exactly what the audit was hunting.

## What was built

**Membership anti-entropy** (the centerpiece):
- `Swim::digest()` — a hash of the entire membership view, carried on every `SWIM_ACK`.
- On persistent digest mismatch, a **push-pull full-state exchange**: `SWIM_SYNC` carries the
  requester's complete view, `SWIM_SYNC_ACK` returns the responder's; one round trip converges both.
- `Swim::fullState()` — every entry *including Dead and Left*, plus self.
- A **resurrection probe**: every 5th tick, one direct ping at a rotating member of the *Dead* set.
  Not in the original design — the new tests forced it (see below).
- `minidynamo_membership_syncs_total` counter; `minidynamo_ring_physical_nodes` gauge
  (`max - min == 0` across the fleet is the divergence health check).
- Harness: `partition()`/`heal()` — unreachable but stateful, which is the difference that makes
  "missed an event" reproducible (kill+restart gets a clean join ack and hides the bug).

## What the tests caught in the design (before it shipped)

Both partition tests failed against the first, plausible-looking implementation — each fixing a
distinct hole. This is the testing tier's discipline paying out at design time:

1. **Mutual-death lockout.** The digest rides acks, and acks only happen when someone probes. A
   partition longer than the suspicion timeout makes BOTH sides expire each other to Dead — and
   nobody probes a Dead node. After healing: no probe → no ack → no digest → the sync that exists
   precisely for this case never triggers, on either side, forever. Hence the **resurrection probe**:
   one direct ping per 5 ticks at a rotating Dead member (never Left — the retired stay retired). A
   real corpse costs one wasted ping; a secretly-alive one answers with a mismatching digest, and
   sync + the standard SWIM self-refutation revive both directions. Direct ping only — escalating an
   expected non-answer through K indirect proxies would waste three round trips per genuinely dead
   member.
2. **The live-decommission piggyback dodge.** The first version of the missed-Leave test
   decommissioned a *running* node — which re-enqueues its own Leave event but, being retired, never
   pings, so the copy sits undrained until the healed node's resurrection probe collects the
   tombstone via ordinary ack piggyback. Convergence without one sync: the `membership_syncs > 0`
   assertion failed and exposed that the test wasn't exercising its own subject. The test now
   decommissions an already-dead node (the canonical use anyway): no event copy survives the drain
   window, the tombstone exists only as *state*, and state travels only by sync.

**Audit quick fixes:**
- Legacy `JOIN` verb deleted (`handleJoin`, `message.{h,cpp}`, stale include). It added any caller
  straight to the router — no Swim, no incarnation, no `Left` tombstone — and nothing sent it
  anymore: a single-frame ring-poisoning hole and a decommission bypass kept alive as dead code.
- `isValidNodeId()` (`src/util.h`): allowlist `[A-Za-z0-9._-]{1,64}`, enforced at NODE_ID startup
  (fatal), the SWIM_JOIN handshake (refused, no ack), and relayed-event application (dropped). An id
  carrying a delimiter corrupts event parsing cluster-wide; one operator typo sufficed.
- Gateway `MAX_FRAME` 64 MB → 16 MB, making its "mirrors framing.h" comment true.
- Runbook: "Decommission a node" section + `scripts/leave.sh` (pure-bash framed LEAVE sender).

## Key design choices

### Digest on every ack + sync on mismatch — not periodic full sync
**Chose:** compare views only when a probe already happens (the ack carries the responder's digest),
and exchange full state only on *persistent* mismatch — 2 consecutive mismatches, max one sync per
peer per 10 protocol periods.
**Rejected:** Serf-style unconditional periodic push-pull. Simpler, but it pays O(state) bandwidth
per period forever on a converged cluster; the digest costs 8 bytes per ack and syncs only when
something is actually wrong. The threshold/cooldown exist because a single mismatch is usually just
an event still in flight — syncing on it would thrash during perfectly normal churn.

### The digest is order-independent and suspicion-blind
Entries are hashed individually and XOR-combined: each node stores *self* outside its member map, so
a stream hash over natural iteration order would differ between two nodes holding **identical**
views. Alive and Suspect collapse to one class — suspicion is a transient per-observer judgement, and
digesting it would cause chronic false mismatches. Self is included as an ordinary entry: the
stranded-partition rescue works precisely because *my* entry for me (Alive) differs from *your*
entry for me (Dead).

### Terminal states digest class-only (incarnation zeroed)
Found while writing the digest, not by a test: two nodes legitimately hold the same Dead member at
*different* incarnations (suspicions expire at different moments; applying a `Leave` keeps the local
incarnation). No sync can reconcile the number — Dead-at-same-or-lower is rejected by design — so a
digest that included it would mismatch and re-sync **forever**, achieving nothing.
`DeadIncarnationDifferencesDigestEqual` pins this. The cost: divergent Dead-incarnations stay
invisible; any class *flip* re-exposes the pair to sync, so nothing correctness-bearing is lost.

### Sync payloads go through `applyEvent`, unchanged
Every existing safety rule gates the sync for free: tombstones win unconditionally and cannot be
resurrected by a diverged peer's stale `Alive@anything`; Alive/Dead need strictly newer incarnations,
so stale sync data regresses no one; and a Dead-about-self triggers the standard SWIM refutation —
which **is** the healed-partition rescue, zero new revival code. One addition: an unknown-node `Dead`
is now recorded (mirror of the unknown-`Leave` tombstone) instead of dropped, because "I hold X:Dead,
you never knew X" was an unresolvable mismatch — every sync replayed the same event, nothing
converged.

### Full state carries tombstones; the join ack still doesn't
`fullState()` (sync) includes Dead and Left; `allMembers()` (join ack) keeps hiding them. A fresh
joiner shouldn't learn of departed nodes — but a sync between two *established* members is exactly
where tombstones must travel: they are the events whose loss is permanent.
`FullStateAndAllMembersDifferOnPurpose` pins the two accessors apart.

### Node-id validation is an allowlist, at three layers
**Chose:** `[A-Za-z0-9._-]{1,64}` at startup (fatal), handshake (refused), and event application
(dropped).
**Rejected:** a denylist of the known delimiters — anything that later becomes a delimiter would
reopen the hole. Three layers because they catch different things: startup stops a typo'd NODE_ID
from ever booting; the handshake stops a bad id entering the stream at its source; apply-time catches
the residue of a corrupted stream (invalid parsed ids, empty ids from malformed tokens). Notably, an
id smuggling `:` mangles the parse into a *truncated but valid-looking* id — only the source-side
checks can stop that vector, which is why apply-time validation alone would not have sufficed.

## Verification

- **151/151 green** in CI's `debian:bookworm-slim`, both partition tests stable over 5 consecutive
  runs, `clang-format-14` clean, node binary links.
- Mutation-proofs (native container FS, fresh builds — never incremental on the bind mount):
  disabling the digest comparison makes both partition tests fail (that mutation reduces the system
  to pre-4.7 event-only dissemination, so it doubles as the proof that both tests pin real
  pre-existing bugs); dropping tombstones from `fullState()` makes
  `PartitionedNodeMissesLeave_SyncConverges` fail. Restore → green.
- The partition test asserts `membership_syncs > 0`, so it cannot silently pass via leftover
  piggyback events instead of the sync path — an assertion that already caught its own first draft
  doing exactly that (see above).

## Where this could still break

1. **Sync storms under pathological churn.** The cooldown caps per-peer sync rate, but a cluster
   whose views *cannot* converge (e.g. a bug that makes digests permanently differ) would sync at the
   cooldown rate forever. `minidynamo_membership_syncs_total` climbing steadily is the alarm; the
   terminal-state incarnation zeroing removed the one such trap found during design.
2. **`SWIM_SYNC` is another unauthenticated verb** on the internal port. It applies events through
   the same gates as normal gossip, so it grants nothing new — but it hands an attacker a convenient
   full-state write vector on a port that was already fully trusted (`REPLICATE`).
3. **Sync payload size is O(members ever seen)** — `members_` never garbage-collects Dead/Left
   entries (audit finding, documented in constraints §2.14). ~40 B/node keeps this trivial up to
   thousands; GC is the eventual fix, deferred because tombstone GC needs care (a GC'd tombstone is a
   reopened revival hole).
4. **The 400 ms drain sleeps in the partition tests** assume the dissemination budget empties within
   ~20 protocol periods. On a pathologically loaded CI box a straggler event could survive, letting
   convergence happen without sync — the `membership_syncs > 0` assertion would then fail the test
   spuriously. If it ever flakes, lengthen the drain, never shorten the assertion.
