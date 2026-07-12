#!/usr/bin/env bash
#
# Run one k6 load profile (bench/load.js) against the running docker-compose stack.
# k6 runs as a container on the compose network and hits the gateway by name, so
# the stack must already be up (docker compose up -d).
#
#   bench/run.sh [label]
#
# Tunables via env: N W R VUS DURATION WRITE_RATIO (see load.js). NET overrides the
# compose network name.
set -euo pipefail
export MSYS_NO_PATHCONV=1   # keep Git-Bash from mangling docker args / the URL (no-op on Linux)
cd "$(dirname "$0")/.."

# The compose network is named "<project>_dhtnet", and the project name derives
# from the checkout directory — so it's "project_mini-dynamo_dhtnet" on the dev
# box but "mini_dynamo_dhtnet" on an EC2 clone. Auto-detect it (override with
# NET=... if you happen to have several *_dhtnet networks).
if [ -z "${NET:-}" ]; then
  NET="$(docker network ls --format '{{.Name}}' | grep -m1 dhtnet || true)"
fi
: "${NET:?no *_dhtnet docker network found — is the stack up? (docker compose up -d)}"
LABEL="${1:-load}"
: "${N:=3}"; : "${W:=2}"; : "${R:=2}"; : "${VUS:=10}"; : "${DURATION:=30s}"; : "${WRITE_RATIO:=0.3}"
: "${KEYPREFIX:=load}"; : "${KEYSPACE:=200}"

echo "=== k6 profile: ${LABEL}  (N=${N} W=${W} R=${R}, VUS=${VUS}, ${DURATION}, write=${WRITE_RATIO}) ==="
docker run --rm -i --network "${NET}" \
  -e N="${N}" -e W="${W}" -e R="${R}" \
  -e VUS="${VUS}" -e DURATION="${DURATION}" -e WRITE_RATIO="${WRITE_RATIO}" \
  -e KEYPREFIX="${KEYPREFIX}" -e KEYSPACE="${KEYSPACE}" \
  -e BASE_URL="http://gateway:8080" \
  grafana/k6 run - < bench/load.js
