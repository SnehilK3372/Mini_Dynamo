#!/usr/bin/env bash
#
# Tier 4.5 scaling benchmark: run the uniform workload at increasing cluster
# sizes and emit a table for bench/scale/RESULTS.md.
#
#   bench/scale/scale_test.sh                  # 5 10 20 50 100
#   SIZES="5 10 20" DURATION=30s bench/scale/scale_test.sh
#
# Run on the swarm MANAGER, after deploy/swarm/deploy.sh.
# Env: SIZES, VUS, DURATION, WRITE_RATIO, KEYSPACE, N, W, R, STACK, AUTH_PASS.
#
# Each size measures: gossip convergence time (how long until every node sees the
# full ring), then throughput/latency once converged. Measuring before
# convergence would time a cluster that doesn't exist yet.
set -euo pipefail
cd "$(dirname "$0")/../.."

STACK="${STACK:-minidynamo}"
NET="${NET:-${STACK}_dhtnet}"
SIZES="${SIZES:-5 10 20 50 100}"
: "${VUS:=100}"; : "${DURATION:=60s}"; : "${WRITE_RATIO:=0.3}"; : "${KEYSPACE:=100000}"
: "${N:=3}"; : "${W:=2}"; : "${R:=2}"
: "${AUTH_USER:=admin}"; : "${AUTH_PASS:=changeme}"
CONVERGE_TIMEOUT="${CONVERGE_TIMEOUT:-180}"
GW="${GW:-localhost:8080}"

log() { printf '\n=== %s ===\n' "$*"; }
RESULTS=()

token() {
  curl -s -X POST "http://$GW/v1/auth/token" -H 'Content-Type: application/json' \
    -d "{\"username\":\"$AUTH_USER\",\"password\":\"$AUTH_PASS\"}" \
    | sed -E 's/.*"token":"([^"]+)".*/\1/'
}

# Ring size as the cluster itself reports it (the gateway's live RING view).
ring_size() {
  local tok="$1"
  curl -s -H "Authorization: Bearer $tok" "http://$GW/v1/cluster/ring" 2>/dev/null \
    | grep -o '"id"' | wc -l | tr -d ' '
}

for size in $SIZES; do
  replicas=$((size - 1))          # the seed is a ring member too → total = replicas + 1
  [ "$replicas" -lt 1 ] && replicas=1
  log "scaling to $size node(s): ${STACK}_kvstore=$replicas + seed"
  docker service scale "${STACK}_kvstore=$replicas" >/dev/null

  TOK=$(token)
  [ -n "$TOK" ] || { echo "auth failed — check AUTH_PASS"; exit 1; }

  # --- gossip convergence: wait until the ring reports every node ---
  start=$(date +%s)
  converged=0
  while [ $(( $(date +%s) - start )) -lt "$CONVERGE_TIMEOUT" ]; do
    seen=$(ring_size "$TOK")
    if [ "${seen:-0}" -ge "$size" ]; then converged=1; break; fi
    sleep 1
  done
  conv=$(( $(date +%s) - start ))
  if [ "$converged" = 1 ]; then
    echo "converged to $size nodes in ${conv}s"
  else
    echo "WARNING: only $(ring_size "$TOK")/$size nodes visible after ${conv}s — recording anyway"
  fi

  # --- load ---
  log "running k6: $size nodes, VUS=$VUS, $DURATION, write=$WRITE_RATIO"
  K6LOG=$(mktemp)
  docker run --rm -i --network "$NET" \
    -e N="$N" -e W="$W" -e R="$R" -e VUS="$VUS" -e DURATION="$DURATION" \
    -e WRITE_RATIO="$WRITE_RATIO" -e KEYSPACE="$KEYSPACE" \
    -e AUTH_USER="$AUTH_USER" -e AUTH_PASS="$AUTH_PASS" \
    -e BASE_URL="http://nginx" \
    grafana/k6 run - < bench/scale/k6_uniform.js >"$K6LOG" 2>&1 || true

  rps=$(grep -E '^\s*http_reqs' "$K6LOG" | grep -oE '[0-9.]+/s' | head -1)
  get_p99=$(grep -E '^\s*get_latency' "$K6LOG" | grep -oE 'p\(99\)=[0-9.]+m?s' | head -1)
  put_p99=$(grep -E '^\s*put_latency' "$K6LOG" | grep -oE 'p\(99\)=[0-9.]+m?s' | head -1)
  errs=$(grep -E '^\s*op_errors' "$K6LOG" | grep -oE '[0-9]+' | head -1)
  echo "size=$size rps=${rps:-n/a} get_p99=${get_p99:-n/a} put_p99=${put_p99:-n/a} errors=${errs:-0} converge=${conv}s"
  RESULTS+=("| $size | ${conv}s | ${rps:-n/a} | ${get_p99:-n/a} | ${put_p99:-n/a} | ${errs:-0} |")
  rm -f "$K6LOG"
done

log "summary (paste into bench/scale/RESULTS.md)"
echo "| Nodes | Convergence | Throughput | GET p99 | PUT p99 | 5xx |"
echo "|-------|-------------|------------|---------|---------|-----|"
printf '%s\n' "${RESULTS[@]}"
