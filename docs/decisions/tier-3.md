# Tier 3 — Minimal AWS deployment (decisions log)

## What was built

Everything needed to stand the stack up on one public EC2 box, reviewable and
runnable, without any change to the application:

- **`deploy/aws/bootstrap.sh`** — OS-detecting (Amazon Linux 2023 / Ubuntu) one-time
  setup: installs Docker + the Compose plugin, clones the repo, seeds `.env`, brings the
  stack up. Idempotent; usable as EC2 *user-data*.
- **`deploy/aws/.env.example`** + `.gitignore` rules — the real secrets (`JWT_SECRET`,
  `AUTH_PASSWORD`, `GRAFANA_PASSWORD`) live in an instance-local `.env` that is never
  committed. Compose reads it automatically.
- **`deploy/aws/README.md`** — the runbook (launch, security group, bootstrap, verify,
  Grafana tunnel, CI secrets, teardown).
- **`.github/workflows/deploy.yml`** — a **manual** (`workflow_dispatch`) SSH deploy job;
  no auto-deploy on merge.
- **README "Deployment (AWS)" section**.

The stack **reuses the existing `docker-compose.yml` unchanged** — the security group,
not a new prod compose file, is the exposure control.

## Key design choices (and the rejected alternative for each)

1. **EC2 + Docker Compose over ECS/Fargate.** The compose file already describes the whole
   topology and runs identically on a laptop and a VM, so a single EC2 box is the *fastest
   path to a live URL* with zero new infra code — exactly the goal of a "minimal" deploy.
   *Rejected:* ECS/Fargate — it wants task definitions, a service, a load balancer, target
   groups, and an image registry (ECR) before anything serves traffic. That's the right tool
   for a real service and is sketched as future work, but it's a lot of YAML for a demo box.

2. **Security group as the exposure control; reuse the dev compose.** Opening only `22`
   (from my IP) and `8080` (the gateway) means every other published port — Grafana,
   Prometheus, node metrics — is unreachable from the internet without touching the compose
   file. Grafana is reached over an SSH tunnel. *Rejected:* a separate hardened prod compose
   that unpublishes those ports — Compose overrides *append* to `ports` (they can't remove a
   mapping), so it wouldn't cleanly do that anyway, and the SG already gives the guarantee.

3. **Container Postgres, not RDS.** The metadata Postgres holds is low-volume and
   demo-scoped (node registry, config versions, an audit log); running it in the compose
   network — never published — keeps the deploy to one box and one command, at zero extra
   cost. *Rejected:* RDS — a managed, backed-up, separately-billed database is the correct
   production choice (durability, failover, patching) but it's a standing cost and more setup
   than a throwaway demo warrants. The gateway already talks to it over JDBC, so the swap is a
   connection-string change when it matters.

4. **t3.medium, not t3.small.** ~7 containers including a JVM (gateway), Grafana, and
   Prometheus don't fit comfortably in 2 GB; 4 GB is the safe minimum. *Rejected:* t3.small —
   cheaper, but OOM-prone under the full stack; a demo that falls over isn't a demo.
   > **Superseded by Tier 4:** the recommendation is now **`m7i-flex.large`** (2 vCPU, 8 GB). Tier 4
   > added per-node gossip, hint, and anti-entropy threads plus a second gateway replica and nginx,
   > which 4 GB no longer holds comfortably. The 2-vCPU burstable ceiling also distorted the Tier-4.3
   > chaos numbers. See `deploy/aws/README.md` and `docs/runbooks/ec2-deploy.md`.

5. **Manual (`workflow_dispatch`) SSH deploy, not auto-deploy on merge.** A portfolio demo
   box should redeploy when *I* choose, not on every commit — and SSH + `docker compose up`
   reuses the exact local workflow with no registry to manage. *Rejected:* auto-deploy on
   merge (surprising, and a red main would take the demo down) and ECR-push/pull (more moving
   parts than one box needs — noted as the natural optimization when builds get heavy).

## What would change to move to Fargate (named, not built)

Build the node + gateway images in CI and push to **ECR**; write **task definitions** (one
per service, or a small set) and an **ECS service** behind an **ALB** with a target group on
the gateway; move Postgres to **RDS** and Prometheus/Grafana to **Amazon Managed Prometheus/
Grafana** (or keep them as sidecar tasks); replace the security-group-on-one-box model with
ALB listener rules + task security groups; and drive it all with **Terraform**. That is a
different tier — it buys managed scaling, rolling deploys, and no SSH, at the cost of
substantial infra code. The cluster's shared-nothing, stateful nodes would also want
**EKS StatefulSets** rather than Fargate to get stable network identity and per-node volumes
(this is the Tier 4 direction).

## Where this could break / honest caveats

- **Plain HTTP.** The gateway serves `:8080` over HTTP, so the JWT is exposed in transit.
  Acceptable for a throwaway demo; a reverse proxy (Caddy → automatic HTTPS with a domain) is
  the small, documented fix and is the first thing to add if the box stays up.
- **Single box = single point of failure**, and the demo's "availability under node loss"
  story is *within* the cluster on that box, not across AZs. Fine for illustrating the
  algorithms; not a resilient deployment.
- **Build-on-instance is heavy.** The node image compiles prometheus-cpp via FetchContent
  (~5 min) and the images plus build cache consume real disk — hence the ≥30 GB root volume
  in the runbook (a too-small disk wedged Docker during Tier 2 development). GHCR-pull would
  remove this from the box.
- **Third-party deploy action.** `appleboy/ssh-action` is pinned to a version; a raw
  `ssh`/`scp` step is the dependency-free alternative if that matters.

## Verification status (honest)

The assistant has **no AWS access**, so the cloud path is operator-run:

- **Locally checked:** `deploy.yml` and `bootstrap.sh` reviewed (`bash -n` clean); the
  `.env` wiring matches the compose variables (`JWT_SECRET`, `AUTH_USERNAME/PASSWORD`,
  `GRAFANA_USER/PASSWORD`); the existing `docker compose up` stack is already proven to come
  up clean (Tier 1C end-to-end).
- **Operator-run (not verifiable here):** launching the EC2 instance, applying the security
  group, running the bootstrap, and confirming the public URL serves the JWT + `PUT/GET`
  flow. The tier's "public URL reachable" definition of done is confirmed by whoever runs the
  deploy — it is **not** claimed as verified by the assistant.
