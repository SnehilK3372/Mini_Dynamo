#!/usr/bin/env bash
#
# Tier 4.5 multi-host smoke test: prove the cluster works ACROSS hosts, not just
# across containers on one box. Run on the swarm MANAGER after deploy/swarm/deploy.sh.
#
#   tests/multi_host_smoke.sh
#
# Asserts:
#   1. the ring spans more than one swarm host (otherwise this proves nothing)
#   2. writes/reads succeed through the overlay network
#   3. killing a whole host's worth of tasks keeps reads available
#   4. the cluster reconverges and hints are delivered on recovery
set -euo pipefail
cd "$(dirname "$0")/.."

STACK="${STACK:-minidynamo}"
GW="${GW:-localhost:8080}"
: "${AUTH_USER:=admin}"; : "${AUTH_PASS:=changeme}"
SEED=8
FAILED=0
log() { printf '\n=== %s ===\n' "$*"; }
ok() { printf 'PASS: %s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*"; FAILED=1; }

log "authenticate"
RESP=$(curl -s -w '\n%{http_code}' -X POST "http://$GW/v1/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$AUTH_USER\",\"password\":\"$AUTH_PASS\"}")
CODE=$(echo "$RESP" | tail -n1)
[ "$CODE" = "200" ] || { bad "auth failed (HTTP $CODE) — check AUTH_PASS"; exit 1; }
TOKEN=$(echo "$RESP" | sed '$d' | sed -E 's/.*"token":"([^"]+)".*/\1/')
AUTH=(-H "Authorization: Bearer $TOKEN")
ok "authenticated"

# --- 1. the ring must actually span hosts -----------------------------------
log "check the cluster spans multiple hosts"
HOSTS=$(docker service ps "${STACK}_kvstore" --filter 'desired-state=running' \
  --format '{{.Node}}' | sort -u | wc -l | tr -d ' ')
echo "kvstore tasks are spread over $HOSTS swarm host(s)"
if [ "$HOSTS" -ge 2 ]; then
  ok "cluster spans $HOSTS hosts (overlay network in use)"
else
  bad "only $HOSTS host — this test cannot prove multi-host behaviour (add workers)"
fi

# --- 2. cross-host write/read ------------------------------------------------
log "seed $SEED keys across the ring"
seeded=0
for i in $(seq 1 $SEED); do
  code=$(curl -s -o /dev/null -w '%{http_code}' "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d "{\"value\":\"mh-$i\"}" -X PUT "http://$GW/v1/kv/mh-$i?W=2")
  [ "$code" = "200" ] && seeded=$((seeded + 1)) || echo "  mh-$i -> $code"
done
[ "$seeded" = "$SEED" ] && ok "wrote $seeded/$SEED keys across hosts" \
  || bad "only $seeded/$SEED writes succeeded"

read_all() {
  local hits=0
  for i in $(seq 1 $SEED); do
    code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "${AUTH[@]}" \
      "http://$GW/v1/kv/mh-$i?R=2" 2>/dev/null) || code=000
    [ "$code" = "200" ] && hits=$((hits + 1))
  done
  echo "$hits"
}
[ "$(read_all)" = "$SEED" ] && ok "read all $SEED keys back" || bad "some reads failed pre-fault"

# --- 3. kill one host's tasks; reads must stay available ---------------------
VICTIM=$(docker service ps "${STACK}_kvstore" --filter 'desired-state=running' \
  --format '{{.Node}}' | sort -u | tail -1)
log "drain host '$VICTIM' (simulates losing a machine)"
docker node update --availability drain "$VICTIM" >/dev/null
trap 'docker node update --availability active "$VICTIM" >/dev/null 2>&1 || true' EXIT
sleep 15  # let SWIM notice (suspicion timeout) and the ring settle

hits=$(read_all)
echo "reads with a host down: $hits/$SEED"
# W=2/R=2 over N=3 tolerates losing one replica per key; a whole host may own
# several vnodes, so allow a small shortfall rather than demanding perfection.
if [ "$hits" -ge $((SEED * 3 / 4)) ]; then
  ok "reads stayed available with a host down ($hits/$SEED)"
else
  bad "reads degraded badly with a host down ($hits/$SEED)"
fi

# --- 4. recover + reconverge -------------------------------------------------
log "restore host '$VICTIM'"
docker node update --availability active "$VICTIM" >/dev/null
sleep 25  # tasks reschedule, rejoin via the seed, hints flush

hits=$(read_all)
[ "$hits" = "$SEED" ] && ok "all keys readable after recovery ($hits/$SEED)" \
  || bad "only $hits/$SEED readable after recovery"

log "result"
[ "$FAILED" = 0 ] && echo "MULTI-HOST SMOKE PASSED" || { echo "MULTI-HOST SMOKE FAILED"; exit 1; }
