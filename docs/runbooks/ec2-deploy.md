# Runbook: Deploy & validate on EC2

End-to-end steps to stand up the full Mini Dynamo stack on a single EC2 instance,
smoke-test the API, verify the Tier-4 features (gossip membership + hinted
handoff), run load and chaos tests, and view metrics in Prometheus/Grafana.

This is the operational companion to [`deploy/aws/README.md`](../../deploy/aws/README.md)
(which covers the launch/security-group specifics). Read that first for the
instance sizing rationale.

> **Scope:** single-host `docker compose` deploy (3 nodes + gateway + Postgres +
> Prometheus + Grafana). Multi-host Swarm is a later tier.

---

## Topology & exposure

The stack is 7 containers. Only **two** ports should ever face the internet:

| Port | Service | Exposure |
|------|---------|----------|
| 22   | SSH | **My IP only** |
| 8080 | gateway (public API) | `0.0.0.0/0` |
| 3000 | Grafana | SSH tunnel only |
| 9090 | Prometheus | SSH tunnel only |
| 9101–9103 | node `/metrics` | SSH tunnel only |
| 5001–5003 | node client TCP | host-published, **not** internet-reachable |
| — | Postgres | never published |

The security group is the exposure control: open only 22 (My IP) and 8080. The
other ports are published to the *host* but blocked from the internet, so you
reach them over SSH tunnels (steps 8–9).

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

Confirm SWIM gossip converged and the Tier-4 threads started (run on the box):

```bash
docker compose logs node1 --tail=30 | grep -iE 'hinted handoff|anti-entropy'
docker compose logs node2 --tail=30 | grep -iE 'gossip|seed|join|alive'
```

Baseline the Tier-4 counters (all start at 0 on a fresh cluster):

```bash
for p in 9101 9102 9103; do
  echo "== node :$p =="
  curl -s localhost:$p/metrics | grep -E \
    'minidynamo_(hints_(stored|delivered)|antientropy_(syncs|keys_repaired))_total'
done
```

`hints_stored` / `hints_delivered` move during the chaos test (step 7). Note:
the anti-entropy cross-node Merkle exchange is currently stubbed
(`docs/decisions/tier-4.2.md`), so `antientropy_syncs_total` may stay flat —
hinted handoff is the Tier-4 feature to demo live.

## 7. Load & chaos tests (run ON the box)

Both scripts run k6 as a container on the compose network, so they must run on
the EC2 host. The network name is auto-detected (override with `NET=...` only if
you have several `*_dhtnet` networks).

```bash
cd ~/Mini_Dynamo

# Load: defaults N=3 W=2 R=2, 10 VUs, 30s, 30% writes.
AUTH_PASS='<AUTH_PASSWORD>' bench/run.sh baseline

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
```

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
| `compose build requires buildx 0.17.0 or later` | Amazon Linux's packaged Docker bundles an old buildx even when Docker itself is already installed. `bootstrap.sh` now upgrades it automatically; on an already-bootstrapped box, re-run `deploy/aws/bootstrap.sh` or manually drop a fresh `docker-buildx` binary into `/usr/libexec/docker/cli-plugins/` (Amazon Linux) or `/usr/lib/docker/cli-plugins/` (Ubuntu/Debian) from the [buildx releases page](https://github.com/docker/buildx/releases), then `chmod +x` it |
| Node won't start / OOM | `docker compose logs node1`; confirm 8 GB, `free -h` |
| k6 "network not found" | `docker network ls \| grep dhtnet` — `bench/run.sh` auto-detects this now; only needed if you have multiple `*_dhtnet` networks |
| k6 all 401 | wrong `AUTH_PASS` — must match `.env` `AUTH_PASSWORD` |
| Prometheus target DOWN | `curl localhost:9101/metrics` on the box |
| Disk full mid-build | `df -h`; prometheus-cpp build needs ≥30 GB headroom |

## One-click redeploys

After the box is bootstrapped once, add repo secrets `EC2_HOST`, `EC2_USER`,
`EC2_SSH_KEY` and use **Actions → "Deploy to EC2 (manual)"**
(`.github/workflows/deploy.yml`) instead of SSHing. Your `.env` on the box is
left untouched; there is no auto-deploy on merge.
