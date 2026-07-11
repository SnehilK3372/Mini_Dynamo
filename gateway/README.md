# Mini Dynamo Gateway

A Spring Boot 3 (Java 17) REST/JWT gateway in front of the Mini Dynamo C++ cluster.
It authenticates callers, validates requests, forwards key-value operations to the
cluster over its TCP wire protocol, and keeps cluster metadata in PostgreSQL.

## Architecture

```
HTTP client ──JWT──▶ Gateway (controller → service → repository)
                        │                      │
                        │ TCP (framed)         │ JDBC/JPA
                        ▼                      ▼
                   C++ cluster            PostgreSQL
              (values, quorum,        (nodes, config_versions,
               vector clocks)              audit_log)
```

- `cluster/ClusterClient` speaks the cluster's length-prefixed, pipe-delimited,
  base64-value protocol (mirrors `src/net/framing.cpp` and `src/base64.cpp`).
- JWT (HS256) is validated by a filter before any controller; `/v1/kv/**` and
  `/v1/cluster/**` require a token, `/v1/auth/token` and health/docs are public.
- Flyway owns the Postgres schema; JPA runs in `validate` mode against it.

## Endpoints

| Method | Path | Notes |
|--------|------|-------|
| POST | `/v1/auth/token` | issue a JWT (public) |
| PUT | `/v1/kv/{key}` | write; body `{value, clock?}`; query `N,W,R` |
| GET | `/v1/kv/{key}` | read; query `N,R`; 404 if absent, 409 with siblings |
| DELETE | `/v1/kv/{key}` | tombstone delete; query `clock?,N,W` |
| GET | `/v1/cluster/nodes` | node registry (Postgres) |
| GET | `/v1/cluster/ring` | live ring (cluster `RING` command) |

Quorum-not-met → `503`; concurrent siblings → `409`; unreachable cluster → `503`.
Swagger UI: `/swagger-ui.html`. Prometheus: `/actuator/prometheus`.

## Run

The whole stack (cluster + Postgres + gateway) comes up from the repo root:

```bash
docker compose up --build
# gateway on http://localhost:8080
```

Example flow:

```bash
TOKEN=$(curl -s -X POST localhost:8080/v1/auth/token \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"changeme"}' | jq -r .token)
curl -X PUT localhost:8080/v1/kv/greeting -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' -d '{"value":"hello"}'
curl localhost:8080/v1/kv/greeting -H "Authorization: Bearer $TOKEN"
```

Configuration is via environment variables (see `application.yml`): `JWT_SECRET`,
`AUTH_USERNAME`/`AUTH_PASSWORD`, `DB_URL`/`DB_USERNAME`/`DB_PASSWORD`, `CLUSTER_NODES`.

## Test

```bash
./mvnw test
```

Runs unit tests (JUnit 5 + Mockito), the wire-codec test, and a Testcontainers
integration test that stands up a real PostgreSQL and drives the HTTP API with
REST Assured (Docker required).

> **Local note (Docker Desktop 29.x):** Testcontainers' bundled docker-java
> defaults to Docker API 1.32, which very new daemons (minimum API 1.40) reject
> with HTTP 400. If the integration test reports *"Could not find a valid Docker
> environment"*, pass the API version to the test JVM:
> `./mvnw test -DargLine=-Dapi.version=1.44`. CI on standard Linux runners does
> not need this.
