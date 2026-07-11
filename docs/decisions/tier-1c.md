# Tier 1C — Observability (decisions log)

## What was built

Metrics and structured logs for both halves of the system, wired end-to-end into a
Prometheus + Grafana stack that comes up with the rest of `docker-compose`.

### Metrics (Prometheus + Grafana)
- **C++ nodes now expose `/metrics`.** The Tier-1A `Metrics` seam grew from a single
  read-repair counter to the full observable surface — request rate by op, request
  latency histogram, and quorum success/failure by op — and a new node-only
  `PrometheusMetrics` backs it with `prometheus-cpp`, standing up a civetweb scrape
  endpoint on `METRICS_PORT` (default 9100). `InMemoryMetrics` still implements the
  same interface for tests, so `kv_core` and GoogleTest take no Prometheus dependency.
- **Instrumentation points.** Request rate + latency are measured at the protocol edge
  (`Node::handleRequest`) for the client-facing PUT/GET/DELETE verbs; quorum
  success/failure is recorded inside the coordinator where the W/R outcome is actually
  known (`writeQuorum`, `coordinateGet`); read-repair was already counted. A `node_up`
  gauge is exposed, though the per-node health panel uses Prometheus' own `up`.
- **Gateway** already exposed `/actuator/prometheus`; Tier 1C enables per-URI latency
  **histogram buckets** (`percentiles-histogram.http.server.requests`) so the latency
  panel can derive p50/p95/p99.
- **Prometheus** scrapes the gateway and all three nodes (config in `deploy/prometheus`).
  **Grafana** is fully provisioned from `deploy/grafana` — datasource + a dashboard
  (`mini-dynamo.json`) with request rate, latency percentiles, quorum success rate,
  read-repair rate, and per-node health — so it is populated at compose-up with no clicks.

### Structured JSON logs
- **Gateway:** Logback with the Logstash JSON encoder (`logback-spring.xml`); every line
  is one JSON object with `service`, `request_id`, `level`, `msg`. A highest-precedence
  `RequestIdFilter` seeds `request_id` into MDC (from `X-Request-Id` or a generated UUID,
  echoed back on the response) so every line logged during a request is correlated.
- **C++ nodes:** a small `jlog` layer (`src/log.*`) emits the same JSON envelope
  (`ts`, `service`, `level`, `node_id`) plus either an operation triple
  (`operation`/`key`/`outcome`) or a free-form `msg`. It is backed by **spdlog** when
  present (Debian's `libspdlog-dev`) and by a mutex-guarded stdout writer otherwise, so
  the node still builds and logs in the lean test image. All node stdout — startup and
  request path — is now JSON.
- Logs go to stdout only; **no ELK** (deferred, per the tier).

## Key design choices (and the rejected alternative for each)

1. **Expand the `Metrics` interface rather than reach for Prometheus in the coordinator.**
   The abstract seam keeps `kv_core` (router, coordinator, versioning) and every unit
   test free of any HTTP/Prometheus dependency; the concrete `PrometheusMetrics` lives in
   a node-only translation unit gated by `HAVE_PROMETHEUS`, exactly mirroring how RocksDB
   is gated. *Rejected:* calling `prometheus-cpp` directly from the coordinator — it would
   drag civetweb into the pure-logic library and the test binary for no benefit.

2. **`prometheus-cpp` via CMake `FetchContent` (compression/push off).** There is no
   Debian package for it, and `docker build` has network even though the runtime node
   images do not, so fetching at image-build time is clean and reproducible. Disabling
   push and compression trims the build to just the pull/exposer path and drops the zlib
   dependency. *Rejected:* hand-rolling a Prometheus text endpoint — fewer dependencies,
   but it wouldn't match the tier's named stack and would re-implement exposition format
   and an HTTP server the library already ships.

3. **Prometheus + Grafana over a hosted APM (Datadog/New Relic).** For a portfolio
   project the pull model is self-contained, free, runs in the same compose file, and is
   the industry-standard pairing an interviewer expects to see. *Rejected:* Datadog et al.
   — an external account, an agent, and per-host billing for zero learning-signal gain;
   nothing to show in the repo.

4. **Dashboards and datasource provisioned as code in the repo.** `deploy/grafana/**` is
   mounted into Grafana so the dashboard exists and is populated the moment the stack
   starts — reproducible, reviewable, diffable. *Rejected:* building the dashboard by hand
   in the UI — invisible to version control and lost on volume reset.

5. **Plain JSON logs to stdout, not ELK/Loki.** One structured line per event on stdout is
   the twelve-factor approach; Docker already captures it and it is trivially greppable and
   machine-parseable. A search/aggregation backend is real infrastructure with its own
   scaling story and is explicitly deferred. *Rejected:* wiring ELK now — scope the tier
   doesn't call for, and it would dwarf the rest of the observability work.

6. **spdlog with a stdout fallback for the nodes.** spdlog gives a thread-safe, leveled
   stdout logger and lets the constant `node_id` live in the pattern while per-event fields
   ride the message; the fallback keeps the node buildable where `libspdlog-dev` isn't
   installed (the lean GoogleTest image). *Rejected:* making spdlog mandatory — it would
   break the fast test-build path for a logger the fallback already covers byte-for-byte.

7. **`request_id` via an MDC filter at highest precedence.** Correlating every log line of
   a request (including auth outcomes in the security chain) needs the id set before
   security runs and cleared after, so a pooled thread never leaks it. *Rejected:* passing
   a correlation id through method signatures — invasive and easily forgotten; MDC is what
   the logging framework is built for.

## Where this could break under adversarial conditions

- **Forwarded requests are double-counted.** A PUT/DELETE that lands on a non-primary is
  counted on both the receiving node and the primary it forwards to. That is the honest
  per-node load view, but cluster-wide request-rate math must account for the extra count
  on writes to non-primary owners.
- **Latency histogram buckets are fixed.** The node's buckets top out at 5s and the
  gateway's are Micrometer defaults; a pathological tail beyond the largest bucket lands in
  `+Inf` and blurs p99. Buckets would need tuning against real Tier-2 load numbers.
- **`up`-based health flaps with scrape timing.** Per-node health is Prometheus' scrape
  liveness, not an app health check, so a brief scrape failure or a node restart shows as a
  dip even when the node is fine. It answers "is it scrapable", which is what the panel claims.
- **Log volume is unbounded and per-request.** Every request emits at least one JSON line to
  stdout with no sampling; under Tier-2 load that is a lot of log I/O. Sampling / level
  control is future work, deliberately out of scope here.
- **`request_id` is absent outside a request.** Startup and background lines carry
  `service`/`level`/`msg` but no `request_id` (there is no request in scope). The field is
  present exactly when it is meaningful.

## Verification status (honest)

Everything below ran on this box (Docker daemon up, JDK 21; Maven via the pinned
`maven:3.9-eclipse-temurin-17` container for the Testcontainers path).

- **C++ core — full GoogleTest suite green (37/37)** in a `debian:bookworm-slim`
  container with RocksDB, built `-DWITH_PROMETHEUS=OFF` (tests link `kv_core`, which
  takes no Prometheus dependency). The expanded `Metrics` interface is source-compatible
  with the existing tests; no Tier-1A/1B regression.
- **Node binary builds with the full observability stack** (`prometheus-cpp` via
  FetchContent + `libspdlog-dev`) in the real `Dockerfile`, and at runtime the node
  serves `/metrics` with every series present — `minidynamo_requests_total{op}`,
  `minidynamo_quorum_total{op,outcome}`, `minidynamo_read_repair_total`,
  `minidynamo_request_latency_seconds_bucket{op,le}`, `minidynamo_node_up` — and emits
  JSON log lines on stdout (startup and request events).
- **Gateway — full suite green (28/28)** including the Testcontainers PostgreSQL
  integration test, with the JSON-logging changes and the `request_id` filter in place.
  (One pre-existing Tier-1B unit test, `JwtServiceTest.tamperedTokenIsRejected`, was flaky
  — it flipped an insignificant low-order bit of the base64url HS256 signature; fixed to
  tamper a high-order signature byte so it invalidates deterministically.)
- **End-to-end on the full `docker compose` stack** (nodes + Postgres + gateway +
  Prometheus + Grafana): confirmed after this commit — Prometheus scraping all four
  targets, the provisioned Grafana dashboard populated, killing a node dropping its
  `up` line and driving a read-repair spike on the next reads, and parseable JSON logs
  from both services. See the Tier 1C session log / a follow-up note for the captured output.
