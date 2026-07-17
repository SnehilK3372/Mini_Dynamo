# Runbook: Deploy & validate on EC2

End-to-end steps to stand up the full Mini Dynamo stack on a single EC2 instance,
smoke-test the API, verify the Tier-4 features (gossip membership + hinted
handoff), run load and chaos tests, and view metrics in Prometheus/Grafana.

This is the operational companion to [`deploy/aws/README.md`](../../deploy/aws/README.md)
(which covers the launch/security-group specifics). Read that first for the
instance sizing rationale.

> **Scope:** single-host `docker compose` deploy. Multi-host Docker Swarm (for the
> scaling benchmark) is `deploy/swarm/` — see [§11](#11-multi-host-swarm-optional).

> ### ⚠️ Read this first: 4.4 + 4.5 require a **fresh cluster**
> Tier 4.4 changed the ring hash and Tier 4.5 changed the vector-clock wire format.
> Both reinterpret existing data, and **every node must run the same build** — a
> mixed-build cluster splits key placement. Before deploying this version:
> ```bash
> cd ~/Mini_Dynamo && docker compose down -v
> ```
> Existing keys are not migrated; they become unreachable under the new hash. That
> is expected for this project's redeploy model, not a bug.

---

## Topology & exposure

The stack is 9 containers (Tier 4.4 added **nginx** and a **second gateway
replica**). Only **two** ports should ever face the internet:

| Port | Service | Exposure |
|------|---------|----------|
| 22   | SSH | **My IP only** |
| 8080 | **nginx** → round-robins to the gateway replicas | `0.0.0.0/0` |
| 3000 | Grafana | SSH tunnel only |
| 9090 | Prometheus | SSH tunnel only |
| 9101–9103 | node `/metrics` | SSH tunnel only |
| 5001–5003 | node client TCP | host-published, **not** internet-reachable |
| — | gateway (×2) | **not** host-published — nginx fronts them |
| — | Postgres | never published |

`:8080` is now **nginx**, not the gateway directly — the gateway has no host port
and no fixed container name so it can scale. Everything else is unchanged: the
security group opens only 22 (My IP) and 8080; the rest is reached over SSH
tunnels ([§8](#8-prometheus-ssh-tunnel)–[§9](#9-grafana-ssh-tunnel)).

**Request path:** `client → nginx:8080 → gateway (×2) → node that owns the key`.
Since 4.4 the gateway hashes the key locally and routes **straight to its primary
owner**, so coordination spreads across all nodes instead of funnelling onto node1.

---

## 1. Launch the instance

Per `deploy/aws/README.md`:

- **AMI:** Amazon Linux 2023 (or Ubuntu 22.04+)
- **Type:** `m7i-flex.large` (2 vCPU, 8 GB) — Tier 4 adds gossip + hint + anti-entropy threads; 4 GB risks OOM under load.
- **Storage:** ≥ 30 GB gp3 (the node image compiles `prometheus-cpp` from source).
- **Security group:** inbound 22 (My IP) + 8080 (0.0.0.0/0) only.

## 2. Bootstrap

```bash
ssh -i your-key.pem ec2-user@<public-ip>          # 'ubuntu@' on Ubuntu AMIs
curl -fsSL https://raw.githubusercontent.com/SnehilK3372/Mini_Dynamo/main/deploy/aws/bootstrap.sh | bash
```

Installs Docker + Compose, clones to `~/Mini_Dynamo`, seeds `.env`, brings the
stack up. First run compiles the node image (~5 min; cached after). It warns if
RAM < 6 GB.

## 3. Set secrets, restart

```bash
cd ~/Mini_Dynamo
nano .env    # JWT_SECRET (openssl rand -base64 48), AUTH_PASSWORD, GRAFANA_PASSWORD
docker compose up -d
```

> If you change `AUTH_PASSWORD` from the default, pass `AUTH_PASS=...` to the load
> scripts in step 6 (they default to `changeme`).

## 4. Verify health

```bash
docker compose ps                         # 7 services running/healthy
curl -s localhost:8080/actuator/health    # {"status":"UP"}
```

## 5. Smoke-test the API

```bash
HOST=<public-ip>
TOKEN=$(curl -s -X POST http://$HOST:8080/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<AUTH_PASSWORD>"}' | jq -r .token)

curl -X PUT http://$HOST:8080/v1/kv/hello -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"value":"world"}'
curl http://$HOST:8080/v1/kv/hello -H "Authorization: Bearer $TOKEN"
curl -X DELETE http://$HOST:8080/v1/kv/hello -H "Authorization: Bearer $TOKEN"
```

- Swagger UI: `http://<public-ip>:8080/swagger-ui.html`

> **HTTP caveat:** the gateway serves plain HTTP — the JWT travels unencrypted.
> Fine for a demo; front it with TLS (Caddy/nginx) for anything real.

## 6. Verify Tier-4 features

Confirm the stack came up as expected (run on the box):

```bash
docker compose ps                      # expect 2 gateway replicas + nginx
curl -s localhost:8080/nginx-health    # "ok" → the LB itself is up
docker compose logs node1 --tail=30 | grep -iE 'hinted handoff|anti-entropy|workers='
docker compose logs node2 --tail=30 | grep -iE 'gossip|seed|join|alive'
docker compose logs --tail=20 | grep -i 'ring'   # gateway ring polling
```

Baseline the Tier-4 counters (all start at 0 on a fresh cluster):

```bash
for p in 9101 9102 9103; do
  echo "== node :$p =="
  curl -s localhost:$p/metrics | grep -E \
    'minidynamo_(hints_(stored|delivered)|antientropy_(syncs|keys_repaired)|pool_connections_(created|reused))_total'
done
```

What to expect, honestly:

| Counter | Expectation |
|---|---|
| `pool_connections_reused_total` | should **dominate** `_created_total` under load (4.3 pooling working) |
| `hints_stored/_delivered_total` | move during the chaos test (step 7) — the Tier-4 feature to demo live |
| `antientropy_syncs_total` | **stays 0 — expected, not a bug.** The cross-node Merkle exchange is still stubbed (`docs/decisions/tier-4.2.md`, `docs/scalability-constraints.md` §2.1) |

**Ring-aware routing check** — coordination should now spread across *all* nodes
rather than piling onto node1. After some load, compare per-node request counts:

```bash
for p in 9101 9102 9103; do
  printf "node :%s  " "$p"
  curl -s localhost:$p/metrics | awk '/^minidynamo_requests_total/{s+=$2} END{print s+0}'
done
```
Roughly balanced ⇒ 4.4 routing is working. Heavily skewed to node1 ⇒ the gateway's
ring is empty/stale and it's falling back to the seed list (still correct, just
un-optimized — see `docs/scalability-constraints.md` §2.9–2.10).

## 7. Load & chaos tests (run ON the box)

Both scripts run k6 as a container on the compose network, so they must run on
the EC2 host. The network name is auto-detected (override with `NET=...` only if
you have several `*_dhtnet` networks).

> **Credentials — the #1 cause of a failed run.** The scripts default to
> `admin`/`changeme`, which only works if you left `AUTH_PASSWORD` at its
> compose fallback. If you set a real password in `.env` (step 3), you **must**
> pass it via `AUTH_PASS` (and `AUTH_USER` if you changed the username) or every
> request 401s. Export it once so every command below inherits it:
>
> ```bash
> export AUTH_PASS="$(grep -oP '^AUTH_PASSWORD=\K.*' ~/Mini_Dynamo/.env)"
> # sanity check — expect 200:
> curl -s -o /dev/null -w '%{http_code}\n' -X POST localhost:8080/v1/auth/token \
>   -H 'Content-Type: application/json' \
>   -d "{\"username\":\"admin\",\"password\":\"$AUTH_PASS\"}"
> ```

```bash
cd ~/Mini_Dynamo

# Load: defaults N=3 W=2 R=2, 10 VUs, 30s, 30% writes. (AUTH_PASS inherited from
# the export above.)
bench/run.sh baseline

# Sweeps (env tunables: N W R VUS DURATION WRITE_RATIO KEYSPACE)
VUS=50 DURATION=60s bench/run.sh heavy
WRITE_RATIO=0.7 VUS=30 bench/run.sh write-heavy
W=1 bench/run.sh w1                 # fast/weak
W=3 R=3 bench/run.sh strong         # strong/slower

# Chaos: kill node2 mid-load, write during outage, restart, assert availability
# + read-repair convergence. One command.
bench/chaos.sh
```

k6 prints throughput (`http_reqs`) and separate `put_latency` / `get_latency`
percentiles. Compare against `bench/RESULTS.md`.

While chaos runs, watch hints climb in a second SSH session:

```bash
watch -n2 'for p in 9101 9102 9103; do curl -s localhost:$p/metrics \
  | grep -E "minidynamo_hints_(stored|delivered)_total"; done'
```

## 8. Prometheus (SSH tunnel)

```bash
# from your laptop
ssh -i your-key.pem -L 9090:localhost:9090 ec2-user@<public-ip>
# open http://localhost:9090   → Status → Targets (gateway + 3 nodes UP)
```

Useful PromQL:

```promql
# throughput per node, by op
sum(rate(minidynamo_requests_total[1m])) by (node_id, op)

# p99 latency (gateway histogram)
histogram_quantile(0.99, sum(rate(minidynamo_request_latency_seconds_bucket[5m])) by (le, op))

# quorum failures (availability signal)
sum(rate(minidynamo_quorum_total{outcome="failure"}[1m])) by (op)

# Tier-4 signals
minidynamo_hints_stored_total
minidynamo_hints_delivered_total
minidynamo_read_repair_total

# Connection pooling (4.3) — reuse should dominate creation
sum(rate(minidynamo_pool_connections_reused_total[1m]))
sum(rate(minidynamo_pool_connections_created_total[1m]))

# Ring-aware routing (4.4): coordination spread, not funnelled onto one node
sum(rate(minidynamo_requests_total[1m])) by (node_id)
```

> **Targets:** the gateway job now uses Docker DNS discovery (`dns_sd_configs`)
> because it has multiple replicas — **Status → Targets should list both gateway
> instances**. If only one appears, the second replica didn't start (see the
> `deploy.replicas` note in Troubleshooting).

## 9. Grafana (SSH tunnel)

```bash
ssh -i your-key.pem -L 3000:localhost:3000 ec2-user@<public-ip>
# open http://localhost:3000  (GRAFANA_USER / GRAFANA_PASSWORD)
```

The Mini-Dynamo dashboard auto-provisions from
`deploy/grafana/dashboards/mini-dynamo.json`. Run a load test in parallel to
watch per-node rate/latency/quorum panels move.

## 10. Teardown / cost

`m7i-flex.large` is not free-tier (~$0.07/hr). When done:

```bash
docker compose down    # stop stack, keep the box
# AWS console: Stop (keeps disk) or Terminate (deletes)
```

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| **Keys written before this deploy are gone / 404** | **Expected.** 4.4 changed the ring hash and 4.5 the clock format — old data is unreachable. You must `docker compose down -v` and start fresh (see the banner at the top) |
| **Only one gateway replica running** | Older `docker compose` ignores `deploy.replicas`. Force it: `docker compose up -d --scale gateway=2`. Harmless if not — nginx just fronts one replica |
| **Requests all pile onto node1** | The gateway's ring is empty or stale, so it's using the seed-list fallback. Check gateway logs for ring polling; confirm node `VNODES` (128) matches gateway `cluster.virtual-nodes` (128) — a mismatch silently disables ring routing |
| **`antientropy_syncs_total` stuck at 0** | Expected — cross-node Merkle exchange is stubbed (`docs/scalability-constraints.md` §2.1). Not a fault |
| `502` from nginx | A gateway replica is down/restarting; `docker compose logs gateway`. nginx caches DNS for 10 s, so a just-removed replica can 502 briefly |
| `compose build requires buildx 0.17.0 or later` | Amazon Linux's packaged Docker bundles an old buildx even when Docker itself is already installed. `bootstrap.sh` now upgrades it automatically; on an already-bootstrapped box, re-run `deploy/aws/bootstrap.sh` or manually drop a fresh `docker-buildx` binary into `/usr/libexec/docker/cli-plugins/` (Amazon Linux) or `/usr/lib/docker/cli-plugins/` (Ubuntu/Debian) from the [buildx releases page](https://github.com/docker/buildx/releases), then `chmod +x` it |
| Node won't start / OOM | `docker compose logs node1`; confirm 8 GB, `free -h`. The stack is now 9 containers (nginx + 2 gateways) |
| k6 "network not found" | `docker network ls \| grep dhtnet` — `bench/run.sh` auto-detects this now; only needed if you have multiple `*_dhtnet` networks |
| k6 all 401 | wrong `AUTH_PASS` — must match `.env` `AUTH_PASSWORD` |
| Prometheus target DOWN | `curl localhost:9101/metrics` on the box |
| Disk full mid-build | `df -h`; prometheus-cpp build needs ≥30 GB headroom |

## Tunables added by Tier 4

All optional — defaults are sane. Set in `docker-compose.yml` env or `.env`.

| Var | Default | Meaning |
|---|---|---|
| `WORKER_THREADS` | 64 | server handler pool size (4.3); connection-bound |
| `POOL_MAX_CONNS_PER_PEER` | 4 | per-peer connection pool cap (4.3) |
| `POOL_IDLE_REAP_SECONDS` | 60 | idle pooled-connection TTL (4.3) |
| `MAX_CLOCK_ENTRIES` | 20 | vector-clock bound (4.5). **Don't lower aggressively** — see `docs/decisions/tier-4.5.md` |
| `VNODES` | 128 | must match the gateway's `cluster.virtual-nodes` |
| `GATEWAY_REPLICAS` | 2 | gateway replicas behind nginx (4.4) |
| `GOSSIP_*` | see `main.cpp` | SWIM period / timeout / K / suspicion (4.1) |

## 11. Multi-host Swarm (optional)

For the 5→100 scaling benchmark across several hosts — **this costs real money**
(3–5 instances). Not needed for a normal demo.

```bash
# on the MANAGER host
deploy/swarm/init-swarm.sh          # inits swarm + a published registry; prints the worker join cmd
# ...run that join command on each WORKER, then back on the manager:
deploy/swarm/deploy.sh              # builds + pushes images, deploys the stack
docker node ls && docker stack services minidynamo

tests/multi_host_smoke.sh           # proves the ring spans hosts + survives losing one
bench/scale/scale_test.sh           # the 5..100 curve → paste into bench/scale/RESULTS.md

deploy/swarm/deploy.sh down         # TEAR DOWN — then stop/terminate the instances
```

Swarm hosts must reach each other on `2377/tcp`, `7946/tcp+udp`, `4789/udp`
(swarm hosts only — not the internet). The stack replaces node1/2/3 with one
scalable `kvstore` service plus a 1-replica bootstrap `seed`; ring size is
`replicas + 1`. Details: `docs/decisions/tier-4.5.md`.

## One-click redeploys

After the box is bootstrapped once, add repo secrets `EC2_HOST`, `EC2_USER`,
`EC2_SSH_KEY` and use **Actions → "Deploy to EC2 (manual)"**
(`.github/workflows/deploy.yml`) instead of SSHing. Your `.env` on the box is
left untouched; there is no auto-deploy on merge.
