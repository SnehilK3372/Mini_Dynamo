#!/usr/bin/env bash
#
# Build + push the images and deploy the Swarm stack (Tier 4.5). Run on the
# MANAGER, after deploy/swarm/init-swarm.sh.
#
#   deploy/swarm/deploy.sh            # deploy with the default replica count
#   KVSTORE_REPLICAS=20 deploy/swarm/deploy.sh
#
# Env: KVSTORE_REPLICAS, GATEWAY_REPLICAS, MAX_CLOCK_ENTRIES, STACK, REGISTRY.
# Teardown: deploy/swarm/deploy.sh down
set -euo pipefail
cd "$(dirname "$0")/../.."   # repo root

STACK="${STACK:-minidynamo}"
REGISTRY="${REGISTRY:-127.0.0.1:5000}"
export REGISTRY
log() { printf '\n=== %s ===\n' "$*"; }

if [ "${1:-}" = "down" ]; then
  log "removing stack $STACK"
  docker stack rm "$STACK"
  echo "(the registry service and swarm itself are left running; "
  echo " 'docker service rm registry' and 'docker swarm leave --force' to go further)"
  exit 0
fi

if ! docker service inspect registry >/dev/null 2>&1; then
  echo "registry service not found — run deploy/swarm/init-swarm.sh first" >&2
  exit 1
fi

# stack deploy ignores `build:`, so build here and push to the swarm-published
# registry; every host then pulls the identical image. This also enforces the
# "all nodes run one build" rule that the Tier 4.4 ring hash and the Tier 4.5
# clock format both depend on — a mixed-build cluster splits key placement.
log "building images"
docker build -t "$REGISTRY/mini-dynamo-node:latest" .
docker build -t "$REGISTRY/mini-dynamo-gateway:latest" ./gateway

log "pushing to $REGISTRY"
docker push "$REGISTRY/mini-dynamo-node:latest"
docker push "$REGISTRY/mini-dynamo-gateway:latest"

log "deploying stack $STACK (kvstore=${KVSTORE_REPLICAS:-4}, gateway=${GATEWAY_REPLICAS:-2})"
docker stack deploy -c deploy/swarm/docker-stack.yml "$STACK" --with-registry-auth

cat <<EOF

Deployed. Useful next steps:
  docker stack services $STACK
  docker service scale ${STACK}_kvstore=20     # ring size becomes 20 + 1 (the seed)
  curl -s localhost:8080/actuator/health       # via nginx on any swarm host
  bench/scale/scale_test.sh                    # the 5..100 scaling curve

Teardown:  deploy/swarm/deploy.sh down
EOF
