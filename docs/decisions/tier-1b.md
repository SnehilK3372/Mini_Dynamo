# Tier 1B — Gateway, JWT, PostgreSQL Metadata (decisions log)

## What was built

A **Spring Boot 3 / Java 17 gateway** (`gateway/`) in front of the C++ cluster, plus two
small, test-guarded additions to the Tier-1A cluster core that two REST endpoints required.

### Gateway (new Maven module)
- **Layered**: `controller → service → repository`, DTOs (records) kept separate from JPA
  entities, Bean Validation on request bodies. Actuator (`/actuator/health`,
  `/actuator/prometheus`) and springdoc/Swagger UI wired.
- **REST API (`/v1`)**: `POST /auth/token`; `PUT/GET/DELETE /kv/{key}` with `N/W/R` query
  overrides; `GET /cluster/nodes` (Postgres registry) and `GET /cluster/ring` (live cluster).
  Status mapping: missing key → **404**, concurrent siblings → **409**, quorum-not-met or
  unreachable cluster → **503**, bad upstream → **502**, bad body/credentials → **400/401**.
- **JWT auth (Spring Security)**: `JwtService` issues/validates HS256 tokens (short expiry,
  secret from env); `JwtAuthFilter` (a `OncePerRequestFilter`) validates signature + expiry
  before controllers; `/v1/kv/**` and `/v1/cluster/**` require a token, the token endpoint and
  health/docs are public. Stateless — no session store.
- **PostgreSQL via Spring Data JPA**: `nodes`, `config_versions`, `audit_log`. Flyway owns the
  schema (`V1__init.sql`); JPA runs `ddl-auto=validate`. `NodeRegistryService` syncs `nodes`
  from the cluster ring on startup; deletes are written to `audit_log`.
- **Cluster wiring (`ClusterClient`)**: a Java implementation of the cluster's TCP protocol —
  length-prefixed framing (mirrors `src/net/framing.cpp`), base64 values (mirrors
  `src/base64.cpp`), pipe-delimited fields — with per-request node failover and timeouts.

### Cluster core additions (C++)
- **Tombstone DELETE.** `VersionedValue` gained a `deleted` flag (serialized as a trailing
  `|D`, backward-compatible with every value written in Tier 1A). `Coordinator::coordinateDelete`
  writes a tombstone through the *same* W-quorum fan-out as a put (extracted into a shared
  `writeQuorum` helper); a dominant tombstone reads back as `NOTFOUND` but still read-repairs
  stale replicas so the delete converges. New wire verb `DELETE|key|origin|N|W|clock`.
- **Read-only `RING` query.** A new non-mutating verb returning the physical ring (reuses
  `Router::getAllPhysicalNodes()`), distinct from `JOIN` which returns the ring *and* adds the
  caller. This is what `GET /v1/cluster/ring` calls.

## Key design choices (and the rejected alternative for each)

1. **A JVM gateway in front of a C++ cluster (the API-gateway pattern).** The gateway adds
   authentication, validation, an HTTP/JSON contract, OpenAPI docs, and a metadata store the
   raw cluster has no business owning. *Cost:* one network hop and a JVM. *Rejected:* exposing
   the cluster's TCP protocol directly to clients — no auth, no validation, no versioned HTTP
   API, and every client would have to reimplement framing + base64 + clock parsing.

2. **Layered controller/service/repository with DTOs separate from entities.** Keeps the HTTP
   contract, the business logic, and the persistence model independently changeable, and keeps
   JPA entities from leaking into the API surface (where lazy-loading and mapping concerns cause
   real bugs). *Rejected:* controllers talking straight to repositories/entities — faster to
   type, but couples the wire shape to the table shape.

3. **JWT over server-side sessions.** Stateless auth scales horizontally with no shared session
   store, and the HMAC signature is what makes the token tamper-proof; a short expiry bounds the
   blast radius of a leaked token (they can't be revoked). *Rejected:* sessions — simple to
   revoke but require sticky sessions or a shared session store, which a stateless gateway in
   front of an AP store shouldn't need.

4. **PostgreSQL for metadata, not RocksDB.** Node registry, versioned config, and an audit log
   are relational, low-volume, and want ACID transactions and ad-hoc queries — exactly a
   relational DB's job. *Rejected:* reusing the cluster's RocksDB — it's a shared-nothing,
   per-node opaque byte store with no cross-node queries, transactions, or SQL; wrong tool for
   administrative metadata.

5. **DELETE as a cluster-level tombstone, not a gateway-only trick.** A delete must dominate the
   value it removes and converge onto every replica (via replication now, read repair on read),
   exactly like a write — otherwise a surviving replica resurrects the key on the next read.
   Doing it in the cluster keeps the Dynamo semantics honest. *Rejected:* a gateway-only "PUT a
   sentinel value" — it can't be distinguished from a real value a client might legitimately
   write, and it doesn't participate in clock reconciliation as a delete.

6. **A read-only `RING` command instead of reusing `JOIN`.** `JOIN` returns the ring *and
   mutates it* (adds the caller); using it for a read-only snapshot would corrupt membership.
   The new verb is a few lines, reuses the existing accessor, and keeps `/v1/cluster/ring`
   side-effect-free. *Rejected:* deriving the ring only from the Postgres registry — that's the
   registry's job (`/cluster/nodes`); `/cluster/ring` is meant to show the cluster's *actual*
   in-memory ring, so drift between the two is visible.

7. **Backward-compatible tombstone encoding (`…|D` suffix).** A live value serializes
   byte-identically to the pre-tombstone format (still one `|`), so every value already on disk
   from Tier 1A still parses and means the same thing. *Rejected:* a new 3-field-always format
   or a JSON envelope — either would break existing stored values or re-introduce the JSON
   dependency Tier 0 deliberately removed.

## Where this could break under adversarial conditions

- **Values are UTF-8 strings at the REST layer.** The cluster is byte-clean (base64 on the
  wire), but the JSON API models a value as a string, so a caller cannot round-trip arbitrary
  binary through the HTTP layer today. A binary-safe API (base64 request bodies) is a small,
  additive change if ever needed.
- **Tombstones are never garbage-collected.** A deleted key keeps a tombstone forever (needed
  so the delete doesn't get undone by a stale replica). Unbounded delete churn grows storage.
  GC-after-grace-period is future work, and belongs with the deferred anti-entropy machinery.
- **`captureBefore` on delete is best-effort and adds a read.** The audit "before" value comes
  from a GET issued just before the DELETE; under concurrency it may not equal what the delete
  actually superseded, and it costs an extra round trip. It is deliberately non-fatal (failures
  are swallowed) so it never blocks a delete.
- **Single demo credential.** Auth validates one env-configured username/password and always
  issues an `ADMIN` token. That is enough to demonstrate the JWT issue/validate flow; a real
  user store, roles, and per-route authorization are out of scope here.

## Verification status (honest)

Everything below was executed on this box (Docker daemon up, JDK 21; Maven via a pinned 3.9.9).

- **C++ core — full GoogleTest suite green (37/37)**, built with RocksDB in a `debian:bookworm-slim`
  container. Includes 4 new tombstone round-trip tests (incl. backward-compat and pipe-heavy
  data) and 4 new coordinator tests (delete→GET NOTFOUND, delete read-repairs a stale replica to
  the tombstone, concurrent delete-vs-write → siblings, delete W-quorum failure). No Tier-1A
  regression, and the `kvstore` node binary compiles/links with the new `DELETE`/`RING` handlers.
- **Gateway — full suite green (28/28)**: `ClusterClient` wire-codec tests against a fake framed
  TCP server (every response shape incl. siblings, empty value, failover, unreachable),
  `JwtService` unit tests (round-trip / expired / tampered / wrong-secret), `KvService` Mockito
  tests (quorum defaults + overrides, 404/409/503 mapping, delete audit), and a **Testcontainers
  integration test** (real PostgreSQL 16 + REST Assured) covering the PUT/GET/DELETE→404 lifecycle,
  the JWT 401 matrix, bad credentials, and the ring/registry endpoints.
- **End-to-end on the real `docker compose` stack** (cluster + Postgres + gateway): 401 without a
  token; token issued; `PUT` returns a real `node1:1` clock; `GET` returns the value; `DELETE`
  then `GET` → **404** (tombstone converged on the real cluster); `/cluster/ring` lists the three
  live nodes; `/cluster/nodes` returns the Postgres registry synced from the ring; `W=5` (> 3
  replicas) → **503**; Swagger UI, OpenAPI docs, and `/actuator/prometheus` all reachable.
- **Environment note.** On this box's Docker Desktop 29.x (Engine API min 1.40), Testcontainers'
  bundled docker-java defaults to API 1.32 and is rejected with HTTP 400; the integration test is
  run with `-Dapi.version=1.44` (documented in `gateway/README.md`). This is a local-daemon
  quirk, not a code issue — standard Linux CI runners don't need it.
