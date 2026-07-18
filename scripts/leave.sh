#!/usr/bin/env bash
# leave.sh — permanently decommission a node (Tier 4.6's LEAVE verb).
#
#   scripts/leave.sh <host:port-of-any-LIVE-node> <node_id_to_remove>
#
# Send this to any live node, NOT to the node being removed — the usual target is
# a node that is already gone. The receiving node tombstones the departure and
# gossips it to the whole cluster.
#
# Speaks the cluster's framed wire protocol (<byte-length>\n<payload>) over
# /dev/tcp, so it needs nothing but bash. Read the runbook section
# "Decommissioning a node" before using this: never LEAVE the live seed, and a
# retired node id can never rejoin — a replacement needs a fresh NODE_ID.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <host:port> <node_id>" >&2
    exit 2
fi

host="${1%%:*}"
port="${1##*:}"
node_id="$2"

if [ -z "$host" ] || [ -z "$port" ] || [ "$host" = "$port" ]; then
    echo "error: first argument must be host:port (got '$1')" >&2
    exit 2
fi

payload="LEAVE|${node_id}"

# Framed exchange: length line + payload out; length line + response back.
exec 3<>"/dev/tcp/${host}/${port}" || { echo "error: cannot connect to ${host}:${port}" >&2; exit 1; }
printf '%s\n%s' "${#payload}" "$payload" >&3

IFS= read -r resp_len <&3
if ! [[ "$resp_len" =~ ^[0-9]+$ ]]; then
    echo "error: malformed frame header from node: '$resp_len'" >&2
    exit 1
fi
resp="$(head -c "$resp_len" <&3)"
exec 3<&- 3>&-

echo "$resp"
case "$resp" in
    "RESPONSE|OK|left")
        echo "decommissioned: '$node_id' is permanently removed; gossip is spreading the tombstone." >&2
        echo "verify: watch ring_physical_nodes converge to the new count on every node." >&2
        ;;
    "RESPONSE|ERROR|unknown_node")
        echo "no member named '$node_id' at ${host}:${port} — check the id (this protects" >&2
        echo "against tombstoning a typo, which would bar that id from ever joining)." >&2
        exit 1
        ;;
    *)
        exit 1
        ;;
esac
