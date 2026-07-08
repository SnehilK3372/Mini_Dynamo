# Mini Dynamo — Focused 2-Week Build Plan

A trimmed, buildable-in-two-weeks version of the full architecture plan. Everything from the "drop entirely" list is gone **except JWT**, with a **minimal AWS deployment** and a **load test** kept. This is the plan to actually execute.

For the deep "why this tool, not that one" reasoning behind each choice, see the companion document (the full architecture plan). This one is focused on scope, sequencing, and the pieces that changed.

* * *

## 1\. Scope: what's in, what's out

**In scope (the whole build):**

- **Tier 0** — repository cleanup.
    
- **Tier 1A** — the distributed core: durable RocksDB storage, real quorum (W/R), vector clocks, read repair. **This is the priority. Protect it.**
    
- **Spring Boot gateway** — REST API + **JWT auth** + PostgreSQL metadata.
    
- **Observability** — Prometheus + Grafana (metrics) + structured JSON logs.
    
- **Testing + CI** — unit / integration / one e2e test + GitHub Actions.
    
- **Load + chaos** — a k6 load test and a node-kill chaos script.
    
- **Minimal AWS** — a single EC2 instance running the Docker Compose stack, reachable at a public URL.
    

**Deliberately deferred (put these under a "Future work" heading in the README — that framing reads as roadmap, not gap):**

Kubernetes/EKS, Terraform (infrastructure as code), gRPC service boundary, distributed tracing (OpenTelemetry/Jaeger), the full ELK stack, hinted handoff + Merkle-tree anti-entropy + SLOs, and the minor niceties (Trivy scanning, Codecov badge, Helm, autoscaling).

**Why this cut.** Interview impact comes from depth you can defend under questioning, not from feature count. The deferred items are either large time sinks (Kubernetes alone can eat the whole two weeks), redundant paradigms (tracing on top of metrics; ELK on top of the Grafana you're already running), or robustness upgrades to things that already work (gRPC replacing a functioning TCP protocol). Cutting them protects the deep, uncommon core — vector clocks, quorum, read repair — which is the part almost no candidate has and precisely the part an interviewer will dig into.

**One note on logging.** Full ELK is deferred, so logging here is **structured JSON to stdout** (captured by Docker) — free and good practice. If you finish early and want searchable logs, **Grafana Loki** is the lightweight add: the same "centralized, searchable logs in a dashboard" story for a fraction of Elasticsearch's setup and memory cost, and it plugs straight into the Grafana you already have. It's optional, not core.

* * *

## 2\. Target architecture

See the diagram . In one paragraph: REST clients hit a **Spring Boot gateway** (which authenticates with JWT, validates requests, and exposes metrics); the gateway forwards over TCP to the **C++ cluster**, where a coordinator hashes the key onto the ring, finds N owners, writes versioned values (vector clocks) to them, and waits for W acknowledgments; each replica persists durably to **RocksDB**; reads gather R responses, compare clocks, return the current value or conflicting siblings, and repair stale replicas asynchronously. **PostgreSQL** holds cluster metadata via the gateway's JPA layer. The lean platform — **Prometheus + Grafana**, structured logs, **GitHub Actions**, **Docker Compose on a single AWS EC2 box**, and **k6** load/chaos testing — supports it all.

* * *

## 3\. The build, step by step

### Tier 0 — cleanup (~1 day)

- Add a `.gitignore` (`build/`, `*.o`, `*.out`, the compiled binary) and purge the committed build artifacts from history.
    
- Rename the top-level directory `Project: Mini-Dynamo` → `mini-dynamo` (the colon is illegal on Windows, the space breaks scripts).
    
- Delete the dead code: `Node::start()` / `Node::server_loop()` (never called — `main.cpp` does its own JOIN), the unused second store (`store`/`store_mtx`) in `node.h`, the unused FNV `hashKey` in `hash.cpp`, and the unread `config/node_config.json` plus its `nlohmann/json` dependency.
    
- Introduce a small **storage interface** (`StorageEngine` with `put`/`get`/iterate) and make the current in-memory map implement it — so the RocksDB swap in Tier 1A is a one-file change and the node stays testable with an in-memory fake.
    
- Write a real README describing what the system actually does today.
    

### Tier 1A — the distributed core (protect this; roughly all of week 1)

Four pieces, in dependency order:

1. **Durable storage (RocksDB).** Drop RocksDB in behind the Tier-0 storage interface. Each node owns its own RocksDB instance on its own disk (shared-nothing). Data now survives restarts.
    
2. **Real quorum (W/R).** Replace today's fire-and-forget replication (which returns `OK` even when replica writes fail) with a coordinator that sends the write to N replicas and **waits for W acknowledgments** before returning success, and gathers **R responses** on reads. Expose N/W/R per request. `W + R > N` gives strong consistency; below that trades consistency for lower latency and higher availability.
    
3. **Vector clocks.** Store versioned values (data + a `{node → counter}` clock). Compare clocks to tell whether one version **dominates** another (causal descendant → supersede) or they're **concurrent** (a real conflict → keep both as siblings and surface them on read).
    
4. **Read repair.** On a read, after answering the client, asynchronously push the current version to any replica that returned a stale one.
    

Write **GoogleTest** unit tests for the subtle logic as you go — especially vector-clock comparison (dominance vs concurrency) and the quorum arithmetic.

### Tier 1B — gateway + JWT + PostgreSQL (~3 days)

- **Spring Boot gateway**, layered properly: Controller → Service → Repository, with DTOs kept separate from JPA entities, Bean Validation on request bodies, Actuator for health/metrics, and springdoc-openapi for an auto-generated Swagger UI.
    
- **REST API:** `PUT`/`GET`/`DELETE /v1/kv/{key}`, plus admin `GET /v1/cluster/nodes` and `/v1/cluster/ring`. Correct status codes (including a conflict status when siblings exist and `503` when a quorum can't be met).
    
- **JWT auth** (see Section 6 for specifics).
    
- **PostgreSQL metadata** (node registry, config versions, audit log) via Spring Data JPA.
    
- The gateway talks to the cluster over the existing TCP protocol — no gRPC in this scope.
    

### Observability (~1–2 days)

- **Prometheus** scrapes `/metrics` from the gateway (Micrometer via Actuator, automatic) and the C++ nodes (the `prometheus-cpp` library).
    
- **Grafana** dashboard: request latency p50/p95/p99, quorum success rate, read-repair count, per-node health. Built by configuring panels — no code, no frontend.
    
- **Structured JSON logs** to stdout (Logback in Java, spdlog in C++). Optional stretch: add **Loki** for log search in Grafana.
    

### Testing + CI (~2–3 days, overlaps the above)

- **Unit:** GoogleTest (ring, vector-clock compare, quorum math) and JUnit 5 + Mockito (gateway service layer with the cluster mocked).
    
- **Integration:** Testcontainers (spins up a real Postgres and cluster in Docker during the test) and REST Assured (drives the HTTP API).
    
- **One e2e test:** write with W=2, kill a node, read, assert the system stayed available and converged.
    
- **GitHub Actions:** lint → build (CMake + Maven) → unit tests → integration tests → build Docker images, on every push. Work in a PR-based branch workflow so the process shows too.
    

### Load + chaos (~half a day)

- **k6** script: concurrent PUT/GET traffic, reporting throughput and p95/p99. This produces a real number and lets you honestly retire the "high performance" claim.
    
- **Chaos script:** a shell script that kills a node mid-load and shows reads still succeed, then restarts it and shows read repair converging. Cheap, and it demonstrates the project's entire thesis in ten seconds.
    

### Minimal AWS deployment (~half to 1 day)

- Launch a single **EC2** instance (a t3.small or t3.medium — the stack needs a couple of GB of RAM for the cluster, gateway, Postgres, and Prometheus/Grafana). Install Docker + Docker Compose, clone the repo, `docker-compose up`. Open the security group for the gateway port (and Grafana, if you want it public). It's now live at the instance's public DNS.
    
- Run **Postgres as a container on the same box** — that's the minimal path. Managed **RDS** is the optional upgrade, not needed here.
    
- **Why EC2 + Compose rather than ECS/Fargate:** you already have a working `docker-compose.yml`, so this is the fastest path to a live URL. Fargate would add task definitions, ECR image pushes, an ALB, and networking you don't need at this timeline. (That's the Tier-2 upgrade in the full plan.)
    
- **Cost caveat:** a t3.small isn't free-tier — it's cheap, but stop or terminate the instance when you're not demoing it.
    

* * *

## 4\. The two-week schedule

**Week 1 — protect the core.**

- **Day 1:** Tier 0 cleanup (gitignore, artifact purge, rename, dead-code removal, storage interface, README skeleton).
    
- **Days 2–4:** RocksDB persistence + real quorum W/R. This fixes the actual correctness gap where writes are currently dropped silently.
    
- **Days 5–7:** vector clocks + read repair, with GoogleTest unit tests written alongside.
    

**Week 2 — the enterprise surface.**

- **Days 8–10:** Spring Boot gateway + JWT auth + PostgreSQL metadata + the REST API + Swagger.
    
- **Days 11–12:** Prometheus + Grafana + structured logging; JUnit/Mockito unit tests and Testcontainers integration tests.
    
- **Day 13:** GitHub Actions CI pipeline; k6 load test + chaos script.
    
- **Day 14:** minimal AWS deployment; finish the README (architecture, run instructions, a short benchmark note, and the "Future work" section); final pass.
    

**If you fall behind — cut from the enterprise surface, never from Tier 1A.** Trim the gateway to a thin REST wrapper (drop the admin endpoints and OpenAPI polish), drop Grafana to just the raw Prometheus `/metrics` endpoint, and skip the AWS deploy (a working `docker-compose up` is an acceptable substitute for a live URL). Protect vector clocks + quorum + read repair above everything else — that's the differentiator; the surrounding tooling is common and replaceable.

* * *

## 5\. What you can honestly claim at the end

- A distributed key-value store with **tunable consistency (N/W/R)**, **vector-clock conflict detection**, **read repair**, and **durable per-node storage (RocksDB)**, in C++.
    
- A **Java / Spring Boot REST gateway** with **JWT authentication** and a **PostgreSQL** metadata store.
    
- **Observability** with Prometheus and Grafana.
    
- A **full test pyramid** (unit / integration / e2e) with **GitHub Actions CI**.
    
- **Load- and chaos-tested**, and **deployed on AWS**.
    

Retire "high performance" and "stress-tested" only once the k6 numbers exist — which they will, on Day 13. The rule throughout: **build it, then claim it.** A tightly-built version of this scope that you understand cold interviews far better than a sprawling half-implementation of the full plan.

* * *

## 6\. JWT specifics (since you're keeping it)

Use standard **Spring Security + JWT** — you're wiring a well-trodden pattern, not inventing anything, which is exactly why it's a cheap keep (~half a day) and defensible in an interview.

**How it works.** A `POST /v1/auth/token` endpoint issues a **signed JWT** (HMAC-SHA256 with a server-side secret, short expiry). Clients send it as `Authorization: Bearer <token>` on every request. A **security filter** validates the signature and expiry *before* the request reaches any controller; the caller's identity and roles come from the token's claims. Because everything needed is in the token, the auth is **stateless** — there's no server-side session store.

**What to build.** The token-issuing endpoint, the JWT validation filter, and route protection so `/v1/kv/**` and `/v1/cluster/**` require a valid token.

**Talking points to have ready** (keep the implementation simple enough that these are all true and you can explain each): stateless auth scales horizontally because there's no session to share across instances; the signature is what makes a token tamper-proof; JWT vs server-side sessions and when you'd choose each; why tokens carry a short expiry. If you can walk through your filter line by line, this is a solid, defensible addition.