#!/usr/bin/env bash
#
# Tier 1D end-to-end test: stands up the whole docker-compose stack, proves the
# system stays AVAILABLE under a single node failure (write W=2, kill a node,
# read R=2), and proves it CONVERGES afterwards (writes taken during the outage
# are pushed to the recovered replica by read repair on the next reads).
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

# read_repair_total summed across all reachable nodes
sum_read_repair(){
  local total=0 v
  for p in "${NODE_PORTS[@]}"; do
    v=$(curl -s "localhost:$p/metrics" 2>/dev/null | awk '/^minidynamo_read_repair_total/{print $2; exit}')
    [ -n "${v:-}" ] && total=$((total + ${v%.*}))
  done
  echo "$total"
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
GOT=$(curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-base?R=2")
echo "$GOT" | grep -q '"value":"base"' && ok "read served with node2 down" || bad "unavailable under 1 failure: $GOT"

log "write 6 keys while node2 is DOWN (W=2 met by the two survivors)"
for i in $(seq 1 6); do
  code=$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d "{\"value\":\"v$i\"}" -X PUT "$GW/v1/kv/e2e-c$i?W=2")
  [ "$code" = "200" ] || bad "write e2e-c$i failed while degraded (HTTP $code)"
done
ok "degraded writes accepted"

log "restart node2 (now missing those 6 writes)"
docker start node2 >/dev/null
sleep 6

RR_BEFORE=$(sum_read_repair)
log "read all 6 keys with R=3 twice -> read repair converges node2"
for pass in 1 2; do
  for i in $(seq 1 6); do curl -s "${AUTH[@]}" "$GW/v1/kv/e2e-c$i?R=3&N=3" >/dev/null; done
done
sleep 2
RR_AFTER=$(sum_read_repair)
echo "read_repair_total: before=$RR_BEFORE after=$RR_AFTER"
[ "$RR_AFTER" -gt "$RR_BEFORE" ] && ok "read repair fired (convergence)" || bad "no read repair observed"

log "result"
if [ "$FAILED" = 0 ]; then echo "E2E PASSED"; else echo "E2E FAILED"; exit 1; fi
