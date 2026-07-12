# Deploying Mini Dynamo to a single EC2 instance

A minimal, reproducible deploy: one EC2 box running the existing `docker compose`
stack, with only the gateway exposed to the internet. ~15 minutes end to end.

> **Cost:** `t3.medium` is **not** free-tier (~$0.04/hr in most regions). **Stop or
> terminate the instance when you're not demoing.**

## 1. Launch the instance

- **AMI:** Amazon Linux 2023 (or Ubuntu 22.04+).
- **Type:** **t3.medium** (4 GB RAM). The stack is ~7 containers — a JVM gateway,
  Grafana, Prometheus, three C++ nodes, and Postgres — so `t3.small` (2 GB) will
  likely OOM. Don't go smaller.
- **Storage:** **≥ 30 GB gp3.** The node image compiles `prometheus-cpp` from source
  and the stack pulls several images; a small root volume fills up and Docker then
  fails to start. Give it headroom.
- **Key pair:** create/select one — you'll SSH with it and reuse it for CI deploys.

## 2. Security group (this is the exposure control)

Inbound rules — open **only** these:

| Type | Port | Source | Why |
|------|------|--------|-----|
| SSH | 22 | **My IP** | admin access + CI deploy |
| Custom TCP | 8080 | 0.0.0.0/0 | the gateway (public API) |

Leave everything else closed. The compose file publishes other ports (Grafana 3000,
Prometheus 9090, node metrics 9101–9103) to the *host*, but with the security group
above they are **not reachable from the internet** — Postgres and the nodes' TCP
protocol are never published at all. Grafana is reached over an SSH tunnel (step 6).

## 3. Bootstrap the box

SSH in (`ssh -i your-key.pem ec2-user@<public-ip>`), then either paste
[`bootstrap.sh`](bootstrap.sh) as the instance **User data** at launch, or run:

```bash
curl -fsSL https://raw.githubusercontent.com/SnehilK3372/Mini_Dynamo/main/deploy/aws/bootstrap.sh | bash
```

It installs Docker + the Compose plugin, clones the repo to `~/Mini_Dynamo`, seeds a
`.env` from the template, and starts the stack. (First run compiles the node image —
~5 minutes; cached afterward.)

## 4. Set real secrets, then restart

```bash
cd ~/Mini_Dynamo
nano .env            # set JWT_SECRET (openssl rand -base64 48), AUTH_PASSWORD, GRAFANA_PASSWORD
docker compose up -d # picks up the new .env
```

## 5. Verify (public API)

```bash
HOST=<public-ip>
TOKEN=$(curl -s -X POST http://$HOST:8080/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"<your AUTH_PASSWORD>"}' | jq -r .token)

curl -X PUT http://$HOST:8080/v1/kv/hello -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"value":"world"}'
curl http://$HOST:8080/v1/kv/hello -H "Authorization: Bearer $TOKEN"
```

- **Swagger UI:** `http://<public-ip>:8080/swagger-ui.html`
- **Health:** `http://<public-ip>:8080/actuator/health`

> **HTTP caveat:** the gateway serves plain HTTP, so the JWT travels unencrypted. Fine
> for a throwaway demo; for anything real put a reverse proxy (e.g. Caddy → automatic
> HTTPS) in front and expose 443 instead of 8080.

## 6. Grafana (private, via SSH tunnel)

Grafana is intentionally not public. Forward it over SSH:

```bash
ssh -i your-key.pem -L 3000:localhost:3000 ec2-user@<public-ip>
# then open http://localhost:3000  (login: GRAFANA_USER / GRAFANA_PASSWORD)
```

## 7. One-click redeploys via GitHub Actions

Add three repo secrets (**Settings → Secrets and variables → Actions**):

| Secret | Value |
|--------|-------|
| `EC2_HOST` | the instance public IP/DNS |
| `EC2_USER` | `ec2-user` (Amazon Linux) or `ubuntu` |
| `EC2_SSH_KEY` | the **private** key PEM contents (the whole file) |

Then **Actions → "Deploy to EC2 (manual)" → Run workflow** (optionally a branch/tag).
It SSHes in, pulls, and `docker compose up -d --build`. Your `.env` on the box is left
untouched. There is deliberately **no auto-deploy on merge**.

## Teardown

```bash
docker compose down          # stop the stack (keep the box)
# or, from the AWS console: Stop (keeps disk, no compute cost) / Terminate (deletes it)
```
