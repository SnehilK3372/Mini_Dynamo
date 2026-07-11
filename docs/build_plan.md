# Mini Dynamo — Build Plan (Executable Checklist)

This is the working plan for Claude Code. Tiers run in order. Do not start tier N+1 until I say so. Read this tier — the whole tier — before entering plan mode for it.

Companion documents:
- Full architectural reasoning: `@docs/architecture.md`
- Behavioral contract (how to work): `@CLAUDE.md`

Working rules recap (also in `CLAUDE.md`):

1. Enter plan mode. Read the tier from this file in full. Produce an implementation plan. Wait for my explicit approval before writing code.
2. Implement fully — production-quality code, comments explaining *why*, tests where the tier calls for them.
3. Run the tests yourself; iterate until green.
4. Write `docs/decisions/tier-<N>.md`: what you built, key design choices, alternative rejected + why for each, one or two places it could break.
5. Tick the checkboxes here as you finish items. Stop. Hand back to me.

Do **not** narrate the code as you build it. Save teaching for a separate review session I'll start later.

---

## Tier 0 — Foundation & cleanup

**Goal:** Clean the repository so the rest of the work has a professional base.

- [x] Add `.gitignore` covering `build/`, `CMakeFiles/`, `Makefile` (root, generated), `cmake_install.cmake`, `CMakeCache.txt`, `*.o`, `*.out`, and the `kvstore` binary.
- [x] Purge the committed build artifacts from git history (not just working tree). Confirm `git log -- build/` returns nothing after. *(No `.git/` existed — fresh `git init` with artifacts deleted before the first commit; history is clean by construction.)*
- [ ] Rename the top-level directory `Project: Mini-Dynamo` → `mini-dynamo` (colon is illegal on Windows, space breaks scripts). Update any references. *(Deferred — user will rename manually; it's the live session's working directory.)*
- [x] Delete dead code, in this order:
  - [x] `Node::start()` and `Node::server_loop()` in `node.cpp` / `node.h`, plus their `SEED_HOST`/`SEED_PORT` reads
  - [x] The unused second store (`store` / `store_mtx`) in `node.h`
  - [x] The unused FNV `hashKey` in `hash.cpp` / `hash.h`
  - [x] `config/node_config.json`
  - [x] `<nlohmann/json.hpp>` include in `node.h` and the JSON library install line in the Dockerfile
  - [x] *(Flagged additions, zero call sites: `Node::sendMessage`, `tcp_send_recv`, `read_length_prefixed`, `split_lines` in `node.cpp`; `split()` in `message.cpp`; unused `jsoncpp` Dockerfile packages.)*
- [x] Introduce a `StorageEngine` abstract interface with `put(key, value)`, `get(key)`, and iterator/scan (design for future versioned values and RocksDB). Make the existing in-memory map an implementation of it. This is the seam Tier 1A will use.
- [x] Write a real `README.md` describing what the system actually does *today* (not the aspirational features). Include: what it is, one-paragraph architecture, `docker-compose up` instructions, wire protocol summary. Link to `docs/architecture.md` for the design.

**Definition of done:** clean `git status`, no build artifacts committed, cluster still comes up via `docker-compose up --build`, README accurately describes the current system, storage interface in place with the in-memory implementation passing existing manual tests.

**Write `docs/decisions/tier-0.md`.**

**Honestly claimable after this tier:** repository hygiene, storage-engine abstraction. Nothing new about the distributed system yet.

---

## Tier 1A — Complete the distributed core

**Goal:** Make the "Dynamo" claim true. Turn the current sharded-with-replication store into a real tunable-consistency, conflict-aware, durable Dynamo. **This is the priority tier — protect it.** Use Fable 5 or Opus 4.8 for this work.

Do the four pieces in this order. Each depends on the previous one.

### 1A.1 — Durable per-node storage (RocksDB)

- [x] Add RocksDB dependency to the build (CMake) and the Dockerfile.
- [x] Implement `RocksDBStorageEngine` behind the Tier 0 interface. Each node owns its own RocksDB directory on its own disk — shared-nothing.
- [x] Wire nodes to use RocksDB by default (`STORAGE_ENGINE` env toggle keeps in-memory available for tests).
- [x] GoogleTest: values survive a node restart; two nodes' storage remain independent. *(Written; run in Docker/CI — no RocksDB on the dev machine.)*

### 1A.2 — Real write quorum (W) and read quorum (R)

- [x] Add `N`, `W`, `R` as configurable per-request parameters (defaults: `N=3`, `W=2`, `R=2`). Extended the wire protocol (now length-prefixed framed; values base64).
- [x] Replace fire-and-forget `REPLICATE` with a request/response that returns an ack. Coordinator waits for `W` acks (with a timeout) before returning `OK`; on timeout, returns `quorum_not_met` for the client to retry.
- [x] On `GET`, coordinator queries the `N` replicas in parallel and waits for `R` responses before answering.
- [x] GoogleTest: `W` met returns OK; `W` not met returns error; a slow ack past the deadline doesn't count and doesn't hang; `R` met returns; downed-replica behavior. *(Run green locally.)*

### 1A.3 — Versioned values with vector clocks

- [x] Define a `VectorClock` type (`map<node_id, counter>`), with a `compare(a, b)` returning `EQUAL | A_DOMINATES | B_DOMINATES | CONCURRENT`. This function is the intellectual heart — make it clean and well-tested.
- [x] Extend the stored value type to `VersionedValue { data, VectorClock clock }`. Updated the wire protocol to carry the clock.
- [x] On PUT: coordinator bumps its own entry (above `max(client context, local stored)`, so a blind write is never born already-dominated) and writes `VersionedValue` to replicas.
- [x] On GET: replicas return their `VersionedValue`; the coordinator compares clocks. If one version dominates, return it. If two are `CONCURRENT`, return all conflicting versions as **siblings**.
- [x] GoogleTest — cover both cases explicitly:
  - [x] Dominance: sequential writes converge to one winner
  - [x] Concurrency: writes with clocks that neither dominate produce siblings on read

### 1A.4 — Read repair

- [x] After answering a `GET`, coordinator asynchronously pushes the dominant `VersionedValue` to any replica whose response was strictly dominated (a stale version). Never blocks the read.
- [x] GoogleTest: stale replica converges to the current version after a read (synchronized via the fake's `waitForWrites`, no sleeps).
- [x] Add a `read_repair_count` counter now, behind a `Metrics` interface (Prometheus wiring comes in the observability tier).

**Definition of done for Tier 1A:** all four sub-tiers implemented, all listed tests passing, `docker-compose up` still runs a healthy cluster, a manual scenario proves the story end-to-end: kill a replica → write with `W=2` succeeds → restart → read triggers repair → replica converges.

**Write `docs/decisions/tier-1a.md`.** Cover explicitly: why quorum over consensus, why vector clocks over last-write-wins, and the read-repair vs hinted-handoff vs anti-entropy trade (with the latter two named as future work).

**Honestly claimable after this tier:** distributed key-value store with durable per-node storage (RocksDB), tunable consistency (N/W/R), vector-clock conflict detection, read repair. **Not yet claimable:** high performance (needs Tier 6 benchmarks), stress-tested (needs Tier 6 load).

---

## Tier 1B — Gateway, JWT, and PostgreSQL metadata

**Goal:** Put a real enterprise service layer in front of the cluster. Sonnet 5 is fine for this tier.

### 1B.1 — Spring Boot gateway skeleton

- [x] New Maven module: Spring Boot 3.x, Java 17. Layered structure: `controller` / `service` / `repository`, with DTOs kept separate from any JPA entities.
- [x] Bean Validation on request bodies (`@Valid`, `@NotNull`, etc.).
- [x] Spring Boot Actuator enabled with `/actuator/health` and `/actuator/prometheus` (Micrometer wiring — the endpoint has to exist for Tier 1C to scrape it).
- [x] springdoc-openapi wired; Swagger UI serving.

### 1B.2 — REST API

- [x] `POST /v1/auth/token` — issues JWT (see 1B.3).
- [x] `PUT /v1/kv/{key}` — write; body carries value and optional client vector clock; query params for `N`, `W`, `R`.
- [x] `GET /v1/kv/{key}` — read; query params for `N`, `R`; response includes value(s) and current clock. If siblings exist, return them with status `409` (or `300` with body; document your choice). *(Chose 409 Conflict.)*
- [x] `DELETE /v1/kv/{key}`. *(Cluster-level tombstone — new `DELETE` verb in the C++ core.)*
- [x] `GET /v1/cluster/nodes` — node registry from Postgres.
- [x] `GET /v1/cluster/ring` — current ring snapshot from the cluster. *(New read-only `RING` verb in the C++ core.)*
- [x] `503` when the cluster can't meet the requested quorum.
- [x] Version the API path (`/v1`).

### 1B.3 — JWT auth via Spring Security

- [x] Spring Security config; JWT filter validating HMAC-SHA256 signature and expiry before any controller.
- [x] Route protection: `/v1/kv/**` and `/v1/cluster/**` require valid token; `/v1/auth/token` and Actuator health are public.
- [x] Short token expiry (15–60 min). Secret comes from env, not source. *(Default 30 min.)*
- [x] JUnit/Mockito: valid token passes; expired token rejected (401); tampered token rejected (401); missing token rejected (401). *(Covered by `JwtServiceTest` + the integration test's 401 matrix.)*

### 1B.4 — PostgreSQL metadata via Spring Data JPA

- [x] Add Postgres to `docker-compose.yml`. Spring Data JPA in the gateway.
- [x] Schema (Flyway `V1__init.sql`, `ddl-auto=validate`):
  - `nodes` — node registry (id, host, port, added_at, last_seen)
  - `config_versions` — versioned cluster config
  - `audit_log` — administrative operations (who, what, when, before/after)
- [x] Repositories, service layer, endpoints wired.
- [x] JUnit/Mockito unit tests; **Testcontainers** integration test against a real Postgres.

### 1B.5 — Gateway ↔ cluster wiring

- [x] Java TCP client that speaks the existing cluster wire protocol; used by the service layer for `PUT`/`GET`/`DELETE` and for admin queries (`GET /v1/cluster/ring`).
- [x] Sensible timeouts and error mapping to HTTP status codes.

**Definition of done:** `docker-compose up` brings up cluster + Postgres + gateway; every REST endpoint works end-to-end; JWT protects the right routes; Testcontainers integration test green; Swagger UI reachable. — **DONE, verified end-to-end.**

**Write `docs/decisions/tier-1b.md`.** Cover: why put a JVM gateway in front of a C++ cluster (the API-gateway pattern, cost is a hop and a JVM, alternative is exposing the cluster protocol directly); why layered architecture; why JWT over sessions; why Postgres for metadata (relational + ACID) rather than reusing RocksDB. — **DONE.**

**Honestly claimable:** Spring Boot REST API, JWT auth, PostgreSQL + JPA + SQL, Testcontainers integration testing.

---

## Tier 1C — Observability

**Goal:** Make the system operable. Sonnet 5.

### 1C.1 — Metrics (Prometheus + Grafana)

- [ ] Prometheus and Grafana in `docker-compose.yml`.
- [ ] Prometheus scrape config: gateway's `/actuator/prometheus` and every C++ node's `/metrics`.
- [ ] `prometheus-cpp` in the C++ nodes exposing: request rate (by op type), latency histograms (p50/p95/p99), quorum success/failure count, read-repair count, per-node up-count.
- [ ] Grafana dashboard (defined as JSON in `deploy/grafana/`) covering:
  - Request rate + latency percentiles (gateway view)
  - Quorum success rate over time
  - Read-repair events over time
  - Per-node health
- [ ] Grafana provisioned automatically at compose-up (datasource + dashboard).

### 1C.2 — Structured JSON logs

- [ ] Java gateway: Logback in JSON encoder; every log line carries at minimum `service`, `request_id`, `level`, `msg`.
- [ ] C++ nodes: spdlog in JSON pattern with the same field shape; include `node_id`, `key` (where meaningful), `operation`, `outcome`.
- [ ] Logs go to stdout (Docker captures them). Do **not** wire ELK in this tier — deferred.

**Definition of done:** Grafana loads with the dashboard populated at compose-up; killing a node visibly moves the "per-node health" panel and produces read-repair spikes on the next reads; `docker logs` output is parseable JSON on both services.

**Write `docs/decisions/tier-1c.md`.** Cover: why Prometheus over Datadog for a portfolio project; why plain JSON logs instead of ELK here (defer rationale); why Grafana dashboards live as JSON in the repo.

**Honestly claimable:** Prometheus + Grafana metrics, structured JSON logging, observability configured as code.

---

## Tier 1D — Testing and CI

**Goal:** A real test suite and an automated pipeline. Sonnet 5.

### 1D.1 — Complete the test pyramid

- [ ] Unit (Java): JUnit 5 + Mockito for the gateway's service layer (with the cluster mocked). Aim for meaningful coverage of the saga-like error paths in 1B.
- [ ] Unit (C++): GoogleTest, if not already added in 1A, covers ring/hashing, vector-clock comparison, quorum arithmetic.
- [ ] Integration: REST Assured driving the HTTP API against a Testcontainers-composed stack (gateway + Postgres + cluster).
- [ ] End-to-end: one test that stands up the whole `docker-compose` stack, writes with `W=2`, kills a node, reads with `R=2`, asserts availability and eventual convergence via read repair.
- [ ] Replace `tests.txt` with a real README section pointing to the automated suite.

### 1D.2 — GitHub Actions CI

- [ ] Workflow triggering on every push and pull request:
  - Lint (clang-format for C++, a formatter check for Java)
  - Build (CMake for C++, Maven for Java)
  - Unit tests (both languages)
  - Integration tests (Testcontainers — matrix on `ubuntu-latest`)
  - Build Docker images
- [ ] Branch protection: main requires a green run.
- [ ] Cache Maven and CMake outputs to keep runs under ~5 minutes.
- [ ] Add a passing-build badge to the README.

**Definition of done:** every push runs the full pipeline; the e2e test proves availability under one node failure; badge is green.

**Write `docs/decisions/tier-1d.md`.** Cover: test pyramid rationale; why Testcontainers over mocks for integration; why GitHub Actions here and the mapping to Jenkins concepts.

**Honestly claimable:** full-lifecycle testing (unit / integration / e2e), CI/CD.

---

## Tier 2 — Load + chaos testing

**Goal:** Real numbers to back the availability thesis. Sonnet 5.

- [ ] k6 script (`bench/load.js`): concurrent PUT/GET traffic, reports throughput and p50/p95/p99. Parameterized on N/W/R.
- [ ] Chaos script (`bench/chaos.sh`): kill a node mid-load, wait, restart it. Assertions/logs prove reads still succeeded during the kill and that read-repair converged the replica after restart.
- [ ] `bench/RESULTS.md`: recorded throughput and latency for `N=3, W=2, R=2` at a couple of concurrency levels; a note on chaos-test behavior. Include a Grafana screenshot if possible.

**Definition of done:** `bench/RESULTS.md` has real numbers; chaos script is runnable in one command.

**Write `docs/decisions/tier-2.md`.** Cover: why k6 over JMeter; what the numbers say about throughput vs quorum tightness (W=1 vs W=2).

**Honestly claimable:** *high-performance* (with the benchmarks to back it), *stress-tested*, empirically-validated availability under node failure.

---

## Tier 3 — Minimal AWS deployment

**Goal:** Live URL. Sonnet 5; parts likely on Haiku 4.5 if truly mechanical.

- [ ] Provision a single EC2 instance (Amazon Linux 2023 or Ubuntu, `t3.small` or `t3.medium`). Document the choice.
- [ ] Security group: expose only the gateway port publicly (and Grafana if you want it public — behind basic auth if so).
- [ ] Install Docker + Docker Compose; clone the repo; `docker-compose up -d`.
- [ ] README section: public URL, how to reach Swagger UI, how to hit the API with `curl` including a JWT flow, cost caveat (t3.small is not free-tier — stop the instance when not demoing).
- [ ] Extend the GitHub Actions workflow with a manual-trigger deploy job (SSH deploy or ECR + pull) — do **not** auto-deploy on every merge yet.

**Definition of done:** anyone with the public URL and a token can hit `/v1/kv/*` and see the metrics dashboard.

**Write `docs/decisions/tier-3.md`.** Cover: why EC2 + Compose over ECS/Fargate for this scope (fastest path, existing compose file reused); why RDS is not used here; what would change to move to Fargate.

**Honestly claimable:** deployed on AWS.

---

## Deferred / future work

Do not build these in the current scope. Named here so the roadmap is honest and interview-ready:

- Hinted handoff and Merkle-tree anti-entropy (complete the AP convergence story)
- ELK stack for log search (Loki as the lighter alternative if pursued)
- Distributed tracing with OpenTelemetry + Jaeger/Tempo
- gRPC + Protobuf as the gateway↔cluster boundary
- Kubernetes (EKS) with StatefulSets for the nodes
- Terraform for infrastructure as code
- SLOs / SLIs / error budgets
- Managed RDS Postgres instead of container-Postgres

---

## The rule underneath all of this

Only claim what a completed, ticked-off tier justifies. If a tier is half-done, the corresponding line doesn't go on the resume or in an interview answer yet. Build it, decision-log it, then claim it.