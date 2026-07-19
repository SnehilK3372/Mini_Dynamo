#!/usr/bin/env bash
#
# End-to-end test: stands up the whole docker-compose stack and proves, on the
# real wire:
#   1. availability under a single node failure (write W=2, kill a node, read R=2)
#   2. convergence afterwards (read repair pushes missed writes to the recovered
#      replica; hinted handoff activity is reported)
#   3. permanent decommission (Tier 4.6): LEAVE evicts a dead node from the ring,
#      observed via the ring_physical_nodes gauge
#   4. membership anti-entropy (Tier 4.7): a node PARTITIONED through the LEAVE
#      misses every gossip event carrying it, and must still converge after the
#      partition heals — digest sync + resurrection probe, not luck
#
# One command:   scripts/e2e.sh
# Env knobs:     COMPOSE_BUILD=0 (reuse existing images)   KEEP_UP=1 (don't tear down)
#
# Exits nonzero on the first failed assertion; used by CI and runnable locally.
set -euo pipefail

cd "$(dirname "$0")/.."
GW=localhost:8080
NODE_PORTS=(9101 9102 9103)   # host-published /metrics ports for node1/2/3
FAILED=0

log(){ printf '\n=== %s ===\n' "$*"; }
ok(){ printf 'PASS: %s\n' "$*"; }
bad(){ printf 'FAIL: %s\n' "$*"; FAILED=1; }

cleanup(){ if [ "${KEEP_UP:-0}" != "1" ]; then log "tearing down"; docker compose down -v >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT

# Sum a counter/gauge across all reachable nodes' /metrics.
sum_metric(){
  local name="$1" total=0 v
  for p in "${NODE_PORTS[@]}"; do
    v=$(curl -s "localhost:$p/metrics" 2>/dev/null | awk -v m="^$name" '$0 ~ m {print $2; exit}')
    if [ -n "${v:-}" ]; then total=$((total + ${v%.*})); fi
  done
  echo "$total"
}
sum_read_repair(){ sum_metric minidynamo_read_repair_total; }

# One node's ring_physical_nodes gauge (arg: host metrics port), or -1 if down.
ring_gauge(){
  local v
  v=$(curl -s "localhost:$1/metrics" 2>/dev/null | awk '/^minidynamo_ring_physical_nodes/{print $2; exit}')
  if [ -n "${v:-}" ]; then echo "${v%.*}"; else echo "-1"; fi
}

# Poll until `ring_gauge PORT` equals WANT, up to TIMEOUT seconds.
wait_ring_gauge(){
  local port="$1" want="$2" timeout="$3" i
  for i in $(seq 1 "$timeout"); do
    [ "$(ring_gauge "$port")" = "$want" ] && return 0
    sleep 1
  done
  return 1
}

log "bring up stack"
if [ "${COMPOSE_BUILD:-1}" = "1" ]; then docker compose up -d --build; else docker compose up -d; fi

log "wait for gateway health"
for i in $(seq 1 60); do
  if curl -sf "$GW/actuator/health" 2>/dev/null | grep -q '"status":"UP"'; then break; fi
  sleep 2
  [ "$i" = 60 ] && { bad "gateway never became healthy"; exit 1; }
done
ok "gateway healthy"

TOKEN=$(curl -s -X POST "$GW/v1/auth/token" -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"changeme"}' | sed -E 's/.*"token":"([^"]+)".*/\1/')
[ -n "$TOKEN" ] && ok "got JWT" || { bad "no token"; exit 1; }
AUTH=(-H "Authorization: Bearer $TOKEN")

log "baseline write (W=2) + read (R=2)"
curl -s "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"value":"base"}' \
  -X PUT "$GW/v1/kv/e2e-base?W=2" >/dev/null
GOT=$(curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-base?R=2")
echo "$GOT" | grep -q '"value":"base"' && ok "baseline round-trip" || bad "baseline read wrong: $GOT"

log "kill node2 -> reads must still succeed (availability under 1 failure)"
docker stop node2 >/dev/null
sleep 5   # let the gateway's connections to node2 settle into fast-fail
GOT=$(curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-base?R=2")
echo "$GOT" | grep -q '"value":"base"' && ok "read served with node2 down" || bad "unavailable under 1 failure: $GOT"

# Writes are coordinated by each key's PRIMARY owner. With node2 down, keys whose
# primary is node2 can't be written — the gateway forwards to the dead primary and
# gets forward_failed after a timeout. Keys primaried on a live node still meet
# W=2 (once gossip confirms node2 dead, the coordinator stores a HINT for it and
# uses a stand-in — sloppy quorum, Tier 4.2/testing-tier). So this is PARTIAL
# write availability: we require *some* writes to succeed, and those successes
# are exactly the versions node2 misses and that hint delivery + read repair
# converge below. --max-time is generous because a forward to the dead primary
# can hang several seconds before failing.
log "write 8 keys while node2 is DOWN (partial write availability)"
succ=0
WROTE=""   # space-separated list of the indices that actually landed
for i in $(seq 1 8); do
  # Capture the status by appending it after the body (portable — avoids
  # -o /dev/null, which some shells mangle); tail takes the trailing code line.
  code=$(curl -s -w '\n%{http_code}' --max-time 20 "${AUTH[@]}" \
    -H 'Content-Type: application/json' -d "{\"value\":\"v$i\"}" \
    -X PUT "$GW/v1/kv/e2e-c$i?W=2" 2>/dev/null | tail -n1) || code=000
  printf '  e2e-c%s -> %s\n' "$i" "$code"
  if [ "$code" = "200" ]; then succ=$((succ + 1)); WROTE="$WROTE $i"; fi
done
echo "degraded writes: $succ/8 succeeded (the rest are keys primaried on the downed node)"
if [ "$succ" -ge 1 ]; then ok "cluster stayed writeable for keys not primaried on node2 (partial availability)"; else bad "no degraded write succeeded"; fi

log "restart node2 (now missing the writes taken while it was down)"
RR_BEFORE=$(sum_read_repair)
HD_BEFORE=$(sum_metric minidynamo_hints_delivered_total)
docker start node2 >/dev/null
sleep 6

# TWO mechanisms converge the missed writes, and they race: hinted handoff
# delivers on the Dead->Alive transition (often winning outright — 6/6 on a fast
# machine), and read repair catches whatever hints didn't cover on the next R=3
# read. Asserting on read repair alone made this test fail precisely when hints
# did their job perfectly, so the assertion is mechanism-agnostic: at least one
# of the two moved data, and — the part that actually matters — every write that
# succeeded during the outage reads back correct afterwards.
log "read all 8 keys with R=3,N=3 twice -> convergence via hint delivery and/or read repair"
for pass in 1 2; do
  for i in $(seq 1 8); do curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-c$i?R=3&N=3" >/dev/null || true; done
done
sleep 2
RR_AFTER=$(sum_read_repair)
HD_AFTER=$(sum_metric minidynamo_hints_delivered_total)
echo "read_repair_total: $RR_BEFORE -> $RR_AFTER   hints_delivered_total: $HD_BEFORE -> $HD_AFTER"
if [ $((RR_AFTER - RR_BEFORE + HD_AFTER - HD_BEFORE)) -gt 0 ]; then
  ok "convergence fired (read repair and/or hint delivery)"
else
  bad "node2 missed $succ writes and neither convergence mechanism moved anything"
fi

log "every degraded write must read back correct at R=3 (end-state, not just counters)"
stale=0
for i in $WROTE; do
  GOT=$(curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-c$i?R=3&N=3" || true)
  echo "$GOT" | grep -q "\"value\":\"v$i\"" || { echo "  e2e-c$i wrong: $GOT"; stale=$((stale + 1)); }
done
[ "$stale" = 0 ] && ok "all $succ outage-era writes converged onto the full replica set" \
  || bad "$stale outage-era write(s) did not converge"

# ---------------------------------------------------------------------------
# Tier 4.6 + 4.7: permanent decommission, observed by a partitioned node.
#
# node2 is cut off BEFORE node3 is decommissioned, so every gossip event carrying
# the LEAVE comes and goes while node2 can't hear — the exact scenario that was
# a permanent, silent divergence before membership anti-entropy: node2 would have
# served a 3-node ring forever. After the partition heals it must converge (learn
# the tombstone, evict node3, be revived by its peers) with NO restart.
# ---------------------------------------------------------------------------
log "Tier 4.6/4.7: decommission node3 while node2 is partitioned"

for p in 9101 9102 9103; do
  wait_ring_gauge "$p" 3 30 || bad "precondition: node on :$p does not report a 3-node ring"
done
ok "all nodes report ring_physical_nodes=3"

NET=$(docker inspect node2 -f '{{range $k,$v := .NetworkSettings.Networks}}{{$k}}{{end}}')
log "partition node2 (docker network disconnect $NET)"
docker network disconnect "$NET" node2

log "stop node3 for good, then LEAVE it via node1"
docker stop node3 >/dev/null
sleep 8   # let gossip on node1 confirm the death (suspicion ~5s) — not required
          # for LEAVE, but models the real operator sequence
LEAVE_RESP=$(bash scripts/leave.sh localhost:5001 node3 2>/dev/null | head -n1) || true
echo "leave.sh -> $LEAVE_RESP"
[ "$LEAVE_RESP" = "RESPONSE|OK|left" ] && ok "LEAVE accepted" || bad "LEAVE failed: $LEAVE_RESP"

wait_ring_gauge 9101 2 60 && ok "node1 evicted node3 from its ring (gauge=2)" \
  || bad "node1 ring gauge never dropped to 2"

# node2 is deaf — and so is its host-published metrics port (port publishing
# rides the same network), so we can't observe its stale gauge from outside
# until the heal. -1 (unreachable) is the expected reading here.
G2=$(ring_gauge 9102)
case "$G2" in
  3)  ok "partitioned node2 still believes in a 3-node ring (gauge=3)" ;;
  -1) echo "info: node2 metrics unreachable during partition (expected — same network)" ;;
  *)  bad "partitioned node2 gauge moved to $G2 — it should not have heard the LEAVE" ;;
esac

log "heal the partition -> node2 must converge with no restart (Tier 4.7)"
docker network connect "$NET" node2

# Budget: resurrection probes fire every 5 ticks (5s) on each side, digest sync
# needs 2 consecutive mismatches at the same peer, and the revival round-trips
# through SWIM self-refutation — comfortably under 90s, typically ~20-40s.
wait_ring_gauge 9102 2 90 \
  && ok "healed node2 learned the missed LEAVE via anti-entropy (gauge=2)" \
  || bad "node2 never converged after the partition healed — the missed LEAVE is a permanent divergence"

echo "info: membership_syncs_total=$(sum_metric minidynamo_membership_syncs_total)"

# Full recovery: reads at R=2 need BOTH surviving ring members, so a 200 with the
# right value proves node2 was revived on node1 (and vice versa) and the gateway
# refreshed its ring — the whole stack healed, not just one gauge.
log "reads must work on the healed 2-node ring (R=2 across node1+node2)"
READ_OK=0
for i in $(seq 1 30); do
  GOT=$(curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-base?R=2" || true)
  if echo "$GOT" | grep -q '"value":"base"'; then READ_OK=1; break; fi
  sleep 2
done
[ "$READ_OK" = 1 ] && ok "post-decommission read round-trip (both survivors answering)" \
  || bad "reads never recovered on the 2-node ring"

log "result"
if [ "$FAILED" = 0 ]; then echo "E2E PASSED"; else echo "E2E FAILED"; exit 1; fi
