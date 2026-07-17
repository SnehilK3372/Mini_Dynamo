#!/usr/bin/env bash
#
# One-time Swarm bootstrap (Tier 4.5). Run on the MANAGER host; it prints the
# command to run on each worker.
#
#   deploy/swarm/init-swarm.sh [advertise-ip]
#
# advertise-ip defaults to the host's primary IP. On EC2 use the instance's
# PRIVATE ip — workers reach the manager over the VPC, and the security group must
# allow, between the swarm hosts only:
#   2377/tcp (cluster management), 7946/tcp+udp (node discovery), 4789/udp (overlay)
set -euo pipefail

ADVERTISE="${1:-$(hostname -I 2>/dev/null | awk '{print $1}')}"
log() { printf '\n=== %s ===\n' "$*"; }

if [ -z "$ADVERTISE" ]; then
  echo "could not determine an advertise IP; pass one explicitly: $0 <ip>" >&2
  exit 1
fi

state="$(docker info --format '{{.Swarm.LocalNodeState}}' 2>/dev/null || echo inactive)"
if [ "$state" = "active" ]; then
  log "swarm already active on this host"
else
  log "initialising swarm (advertise-addr $ADVERTISE)"
  docker swarm init --advertise-addr "$ADVERTISE"
fi

# The registry runs AS a swarm service published on 5000, so the routing mesh
# makes it reachable at 127.0.0.1:5000 from EVERY host. That matters twice over:
# `docker stack deploy` ignores `build:`, so each host must PULL the images, and
# Docker treats 127.0.0.1:5000 as an insecure registry by default — no TLS setup.
if docker service inspect registry >/dev/null 2>&1; then
  log "registry service already running"
else
  log "starting a swarm-published registry on :5000"
  docker service create --name registry --publish published=5000,target=5000 \
    --constraint 'node.role == manager' registry:2 >/dev/null
fi

log "manager ready — run this on each WORKER host"
docker swarm join-token worker | sed -n '3p'

cat <<EOF

Then, back on this manager:
  deploy/swarm/deploy.sh          # build + push images, deploy the stack
  docker node ls                  # confirm the workers joined
EOF
