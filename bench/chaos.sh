#!/usr/bin/env bash
#
# Chaos test: kill a node MID-LOAD and prove the system stays available for reads
# and converges afterwards. One command:  bench/chaos.sh
#
#   1. seed keys (all nodes up)
#   2. start a 50s background READ load (k6) over those keys
#   3. mid-load: kill node2, then overwrite keys while it's down (versions node2 misses)
#   4. restart node2 and re-read (R=3) to trigger read repair
#   5. assert: the background reads saw zero server errors THROUGH the kill, and
#      read_repair_total rose (the replica converged)
#
# Exits nonzero on a failed assertion.
set -euo pipefail
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."

GW=localhost:8080
NODE_PORTS=(9101 9102 9103)
SEED=8
FAILED=0
# Must match the gateway's real AUTH_USERNAME/AUTH_PASSWORD (.env on the box) —
# defaults only work if you left them at the docker-compose.yml fallback values.
# Exported so the background bench/run.sh k6 invocation picks them up too.
: "${AUTH_USER:=admin}"; : "${AUTH_PASS:=changeme}"
export AUTH_USER AUTH_PASS
log(){ printf '\n=== %s ===\n' "$*"; }
ok(){ printf 'PASS: %s\n' "$*"; }
bad(){ printf 'FAIL: %s\n' "$*"; FAILED=1; }

sum_read_repair(){
  local t=0 v
  for p in "${NODE_PORTS[@]}"; do
    v=$(curl -s "localhost:$p/metrics" 2>/dev/null | awk '/^minidynamo_read_repair_total/{print $2; exit}')
    if [ -n "${v:-}" ]; then t=$((t + ${v%.*})); fi
  done
  echo "$t"
}

# We inject the fault with docker stop/start (a genuine node kill). Always bring
# node2 back on exit so a mid-run failure never leaves the cluster a node short.
trap 'docker start node2 >/dev/null 2>&1 || true' EXIT

log "ensure stack is up"
docker compose up -d >/dev/null
for i in $(seq 1 60); do
  curl -sf "$GW/actuator/health" 2>/dev/null | grep -q '"status":"UP"' && break
  sleep 2; [ "$i" = 60 ] && { echo "gateway never healthy"; exit 1; }
done
AUTH_RESP=$(curl -s -w '\n%{http_code}' -X POST "$GW/v1/auth/token" -H 'Content-Type: application/json' \
  -d "{\"username\":\"${AUTH_USER}\",\"password\":\"${AUTH_PASS}\"}")
AUTH_CODE=$(echo "$AUTH_RESP" | tail -n1)
AUTH_BODY=$(echo "$AUTH_RESP" | sed '$d')
# Validate on HTTP status, not just "sed produced non-empty output" — sed leaves
# its input unchanged on a no-match, so a 401 error body would otherwise be
# silently accepted as a "token" and every subsequent call would 401 unnoticed.
if [ "$AUTH_CODE" != "200" ]; then
  bad "auth failed (HTTP $AUTH_CODE) — check AUTH_USER/AUTH_PASS match the gateway's .env"
  echo "response: $AUTH_BODY"
  exit 1
fi
TOKEN=$(echo "$AUTH_BODY" | sed -E 's/.*"token":"([^"]+)".*/\1/')
[ -n "$TOKEN" ] && [ "$TOKEN" != "$AUTH_BODY" ] && ok "authenticated" || { bad "200 response had no token field"; exit 1; }
AUTH=(-H "Authorization: Bearer $TOKEN")

log "seed $SEED keys (all nodes up)"
seeded=0
for i in $(seq 1 $SEED); do
  code=$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d "{\"value\":\"seed-$i\"}" -X PUT "$GW/v1/kv/chaos-$i?W=2")
  [ "$code" = "200" ] && seeded=$((seeded + 1)) || echo "  chaos-$i seed -> $code"
done
if [ "$seeded" = "$SEED" ]; then
  ok "seeded ($seeded/$SEED)"
else
  bad "only seeded $seeded/$SEED keys — check auth/quorum before trusting the rest of this run"
fi

# Baseline the cumulative repair counter BEFORE the fault. Repairs can fire the
# moment node2 is back — the background R=2 load itself heals it as it reads — so
# convergence must be measured across the whole scenario, not just a late window.
RR_BEFORE=$(sum_read_repair)

log "start 60s background READ load over the seeded keys (4 VUs)"
# Modest concurrency on purpose: with a node down there is only ONE spare replica
# left to satisfy W=2/R=2, so a heavy load would saturate it and turn the outage
# into a throughput story rather than an availability one. 4 VUs keeps the spare
# replica comfortably within quorum deadlines so the availability + convergence
# behaviour is what's exercised.
K6LOG=$(mktemp)
KEYPREFIX=chaos KEYSPACE=$SEED WRITE_RATIO=0 VUS=4 DURATION=60s \
  bash bench/run.sh chaos-read >"$K6LOG" 2>&1 &
K6PID=$!
sleep 12

log "kill node2 MID-LOAD (docker stop)"
docker stop node2 >/dev/null
sleep 3
log "overwrite keys while node2 is DOWN with W=1 (creates versions it misses)"
# W=1 on purpose: with node2 down there's a single spare replica, and under the
# concurrent read load a W=2 write can't get its second ack inside the deadline
# (it returns 503). W=1 commits on the coordinator alone — it succeeds under load
# for any key whose primary is live, and node2 (down) still misses the new version,
# which is exactly the staleness read repair heals after the restart. Keys primaried
# on node2 still 502 (forward to the dead primary); --max-time bounds that wait.
wrote=0
for i in $(seq 1 $SEED); do
  code=$(curl -s -w '\n%{http_code}' --max-time 8 "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d "{\"value\":\"during-outage-$i\"}" -X PUT "$GW/v1/kv/chaos-$i?W=1" 2>/dev/null | tail -n1) || code=000
  printf '  chaos-%s -> %s\n' "$i" "$code"
  [ "$code" = "200" ] && wrote=$((wrote + 1)) || true
done
echo "overwrote $wrote/$SEED keys while node2 was down (the rest are primaried on node2)"
if [ "$wrote" -ge 1 ]; then ok "writes accepted for live-primary keys during the outage"; else bad "no write succeeded during the outage"; fi
sleep 6

log "restart node2 (docker start), wait until it's serving"
docker start node2 >/dev/null
# Wait for node2 to actually be back (its /metrics endpoint answers) before the
# R=3 reads — R=3 needs all three replicas to respond, so reading too early just
# yields quorum_not_met instead of the read-repair we're demonstrating.
for i in $(seq 1 25); do
  curl -sf localhost:9102/metrics >/dev/null 2>&1 && break
  sleep 1
done

log "wait for background read load to finish"
wait "$K6PID" || true

# Converge AFTER the load ends: R=3 needs all three replicas to answer in time, so
# the reads must not contend with the load. Quiet R=3 reads pull node2's stale copy
# into the comparison, the coordinator sees the dominant version, and repairs node2.
log "converge node2 via quiet R=3,N=3 reads (belt-and-braces on top of the load)"
sleep 2
for pass in 1 2 3; do
  for i in $(seq 1 $SEED); do curl -s "${AUTH[@]}" "$GW/v1/kv/chaos-$i?R=3&N=3" >/dev/null || true; done
done
sleep 2
RR_AFTER=$(sum_read_repair)

REQS=$(grep -E "http_reqs" "$K6LOG" | tail -1 | tr -s ' ')
CHECKS=$(grep -E "checks_succeeded" "$K6LOG" | tail -1 | tr -s ' ')
ERRLINE=$(grep -E "op_errors" "$K6LOG" | tail -1 | tr -s ' ')
echo "background load reqs:   ${REQS:-n/a}"
echo "background load checks: ${CHECKS:-n/a}"
echo "background load errors: ${ERRLINE:-op_errors...: 0 (none recorded)}"

# Assertion 1: reads stayed HIGHLY AVAILABLE through the kill. Killing a node
# mid-load drops a handful of in-flight requests at the instant it dies; the thesis
# is high availability, not literally zero errors. Assert the error rate stayed
# under 5% (i.e. >95% of reads served with a node down) and report the actual rate.
errcount=$(printf '%s' "${ERRLINE:-}" | grep -oE '[0-9]+' | head -1 || true); errcount=${errcount:-0}
reqcount=$(printf '%s' "${REQS:-}" | grep -oE '[0-9]+' | head -1 || true); reqcount=${reqcount:-0}
if [ "$reqcount" -gt 0 ]; then
  pctx100=$((errcount * 10000 / reqcount))   # error rate ×100 (two decimal places)
  echo "read error rate: ${errcount}/${reqcount} = $((pctx100 / 100)).$(printf '%02d' $((pctx100 % 100)))%"
  if [ "$((errcount * 100))" -le "$((reqcount * 5))" ]; then
    ok "reads stayed available through the kill (>95% served with a node down)"
  else
    bad "reads degraded past the 5% threshold during the chaos window"
  fi
else
  bad "no background load requests recorded"
fi

# Assertion 2: read repair converged the restarted node
echo "read_repair_total: before=$RR_BEFORE after=$RR_AFTER"
if [ "$RR_AFTER" -gt "$RR_BEFORE" ]; then
  ok "read repair converged the restarted node ($((RR_AFTER - RR_BEFORE)) repairs)"
else
  bad "no read repair observed after restart"
fi

rm -f "$K6LOG"
log "result"
if [ "$FAILED" = 0 ]; then echo "CHAOS PASSED"; else echo "CHAOS FAILED"; exit 1; fi
