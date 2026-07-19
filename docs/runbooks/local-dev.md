# Runbook: Build, run & test everything (local)

Every command needed to build, test, run, exercise, and debug Mini Dynamo on a dev machine. The
EC2/production counterpart is [`ec2-deploy.md`](ec2-deploy.md). Commands are for **bash** (Git Bash on
Windows); this repo's reference dev box is Windows + Docker Desktop (WSL2 backend), which is why the
Docker-based flows below are the canonical ones — the node binary itself is POSIX-only.

## 0. Prerequisites

| Tool | Why | Check |
|---|---|---|
| Docker Desktop (WSL2 backend) | C++ builds/tests run in CI's exact image; the stack is compose | `docker info` |
| bash (Git Bash on Windows) | every script (`scripts/`, `bench/`) | `bash --version` |
| Java 17 | gateway build/tests (runs on the host, not in Docker) | `java -version` |
| curl + jq | smoke tests, metrics | `curl --version` |

No local C++ toolchain is required — **all C++ work happens inside `debian:bookworm-slim`**, the same
image CI uses (g++ 12, clang-format **14.0.6**, RocksDB, GoogleTest). That kills every version-drift
problem, most importantly the formatter (CI pins clang-format 14; a newer local one formats
differently and fails lint).

## 1. The four commands that matter

```bash
# 1. Full C++ check — format + build + all unit/property/cluster tests (CI-identical)
scripts/dev-test.sh              # see §2; add FORMAT=fix to auto-format first

# 2. Gateway (Java) tests
cd gateway && ./mvnw test        # unit; `./mvnw verify` adds spotless + integration

# 3. The whole stack, locally
docker compose up --build -d     # 9 containers; API at http://localhost:8080

# 4. Whole-stack end-to-end (builds images, runs failure/decommission/partition drills)
scripts/e2e.sh                   # COMPOSE_BUILD=0 to reuse images, KEEP_UP=1 to keep the stack
```

## 2. C++: build + test (CI-identical)

`scripts/dev-test.sh` wraps the canonical flow. What it does, and the manual equivalent:

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd)":/w:ro debian:bookworm-slim bash -c '
  cp -r /w /src && cd /src        # copy to the container-native FS — see the warning below
  apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    g++ cmake make clang-format librocksdb-dev libgtest-dev libspdlog-dev ca-certificates
  # format check exactly as CI runs it (clang-format 14.0.6)
  find src tests \( -name "*.cpp" -o -name "*.h" \) -print0 | xargs -0 clang-format --dry-run -Werror
  cmake -S . -B /tmp/b -DBUILD_TESTING=ON -DWITH_PROMETHEUS=OFF
  cmake --build /tmp/b -j"$(nproc)"
  ctest --test-dir /tmp/b --output-on-failure
'
```

To **apply** formatting (mount read-write, no copy — format edits must land in the repo):

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd)":/w -w /w debian:bookworm-slim bash -c '
  apt-get update -qq && apt-get install -y -qq --no-install-recommends clang-format
  find src tests \( -name "*.cpp" -o -name "*.h" \) -print0 | xargs -0 clang-format -i
'
```

Run a subset while iterating (after a build):

```bash
/tmp/b/tests/kvstore_tests --gtest_filter='Cluster.*'          # in-process multi-node scenarios
/tmp/b/tests/kvstore_tests --gtest_filter='SwimDigest.*'       # membership digest properties
/tmp/b/tests/kvstore_tests --gtest_filter='VectorClockProperty.*'
```

> **Warning — never do incremental builds on the bind mount.** `make`'s mtime tracking across the
> Windows↔Docker bind mount is unreliable: it silently reuses stale objects, which once mis-scored two
> live mutations as "test passed". Copy the tree to the container FS (as above) or use a fresh build
> dir. A one-shot clean build on the mount is fine.

**Mutation testing** (proving a regression test fails against the bug it targets): the pattern lives
in `docs/decisions/tier-4.7.md` §Verification — copy source to `/src` inside the container, one
baseline build, mutate → incremental rebuild (native FS only) → run the targeted `--gtest_filter` →
expect exit 1 → restore. gtest exit codes are the only signal: 0 = passed, 1 = an assertion failed,
anything else (127, 139…) is *inconclusive*, not proof.

## 3. Gateway: build + test (host-side Java)

```bash
cd gateway
./mvnw test                # unit tests (fast, no Docker)
./mvnw verify              # + spotless format check + integration tests
./mvnw spotless:apply      # fix formatting
```

> **Windows note:** `GatewayIntegrationTest`/`RealClusterIT` use Testcontainers, which cannot reach
> the Docker daemon from the Windows JVM here — they self-skip or fail locally and are covered by CI
> (which builds the node image and runs them for real). Unit tests are the local signal.

## 4. Run the stack

```bash
docker compose up --build -d     # first run builds node + gateway images (~3-5 min)
docker compose ps                # 9 containers: node1-3, postgres, 2× gateway, nginx, prometheus, grafana
docker compose logs -f node1     # structured JSON logs
docker compose down              # stop (keeps node data — but see the fresh-cluster note)
docker compose down -v           # stop + WIPE (fresh ring — required across ring-hash/clock changes)
```

| Endpoint | Where |
|---|---|
| API (nginx → 2× gateway) | `http://localhost:8080` |
| Node TCP (wire protocol) | `localhost:5001..5003` |
| Node metrics | `http://localhost:9101..9103/metrics` |
| Prometheus | `http://localhost:9090` |
| Grafana | `http://localhost:3000` (admin/admin) |

### Smoke test the API

```bash
TOKEN=$(curl -s -X POST localhost:8080/v1/auth/token -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":"changeme"}' | jq -r .token)
AUTH="Authorization: Bearer $TOKEN"

curl -s -H "$AUTH" -H 'Content-Type: application/json' -d '{"value":"hello"}' \
  -X PUT 'localhost:8080/v1/kv/mykey?W=2'                      # write (quorum W=2)
curl -s -H "$AUTH" 'localhost:8080/v1/kv/mykey?R=2' | jq       # read (quorum R=2)
curl -s -H "$AUTH" -X DELETE 'localhost:8080/v1/kv/mykey?W=2'  # versioned tombstone
curl -s localhost:8080/actuator/health | jq                    # gateway health
```

Concurrent writes to the same key without passing back the returned `clock` produce **siblings** on
read — that's vector-clock conflict surfacing, not a bug; resolve by writing again with one of the
returned clocks as context.

## 5. Operational drills (verify the Tier-4 machinery live)

### Kill / recover (temporary failure — ring must NOT change)

```bash
docker stop node2
# reads keep working (sloppy quorum; dead owner skipped):
curl -s -H "$AUTH" 'localhost:8080/v1/kv/mykey?R=2' | jq
# hints accumulate for node2 on the survivors:
curl -s localhost:9101/metrics | grep hints_stored_total
docker start node2               # rejoins on its own; hints deliver on Dead->Alive
curl -s localhost:9101/metrics | grep hints_delivered_total
```

### Decommission (permanent removal, Tier 4.6)

```bash
docker stop node3                                # the node is gone for good
bash scripts/leave.sh localhost:5001 node3       # tell any LIVE node -> RESPONSE|OK|left
# watch every node's ring shrink to 2 (fleet max==min is the health check):
for p in 9101 9102; do curl -s localhost:$p/metrics | grep '^minidynamo_ring_physical_nodes'; done
```

Rules: never LEAVE the live seed; a retired id can never rejoin (replacement machines get a fresh
`NODE_ID`); full rationale in [`ec2-deploy.md` §7b](ec2-deploy.md).

### Partition + heal (membership anti-entropy, Tier 4.7)

```bash
docker network disconnect minidynamo_dhtnet node2   # network name: <project>_dhtnet — check `docker network ls`
# ...do membership changes node2 can't hear (e.g. the decommission above)...
docker network connect minidynamo_dhtnet node2
# node2 converges with NO restart — digest sync + resurrection probe:
curl -s localhost:9102/metrics | grep -E 'ring_physical_nodes|membership_syncs_total'
```

`scripts/e2e.sh` runs exactly this drill with assertions; use it as the executable reference.

## 6. Load & chaos (k6)

```bash
export AUTH_PASS=changeme        # must match the stack's AUTH_PASSWORD
bash bench/run.sh                # load: N=3 W=2 R=2, 10 VUs, 30s, 30% writes
N=3 W=2 R=2 VUS=50 DURATION=60s bash bench/run.sh    # sweep knobs
bash bench/chaos.sh              # kill node2 mid-load, write through it, assert convergence
```

## 7. CI — what a push has to pass

One workflow (`.github/workflows/ci.yml`), four jobs. PRs run each job **once** (push-triggered runs
are limited to `main`; previously PR branches ran everything twice).

| Job | Gate |
|---|---|
| C++ — lint, build, test | clang-format-14 `--dry-run -Werror`, then full ctest in `debian:bookworm-slim` |
| Gateway | spotless + unit + Testcontainers integration (real node image) |
| Build Docker images | production Dockerfiles must build (Prometheus ON) |
| End-to-end | `scripts/e2e.sh`: availability under failure, convergence, decommission, partition-heal |

Reproduce any C++ CI failure locally with §2 — it is the same image, same commands.

## 8. Troubleshooting

- **Docker Desktop crashes at start with `...dockerInference: The file cannot be accessed by the
  system`** — stale unix-socket reparse points in `%LOCALAPPDATA%\Docker\run` from an unclean
  shutdown, and neither PowerShell nor `rm` can delete them. Fix: quit Docker fully, **rename the
  directory aside** (`Rename-Item $env:LOCALAPPDATA\Docker\run run.broken`), relaunch — it recreates
  `run/` and starts clean. Delete the renamed dir whenever. The engine can also **die mid-session**
  the same way (UI still running, `docker info` fails) — and a dying daemon executes commands like
  `docker network connect` *without actually doing them*, which manifests as impossible test
  failures. If e2e fails weirdly, check `docker info` **before** debugging the code. Disabling the
  "Docker Model Runner" (inference) feature in Docker Desktop settings stops the recurrence.
- **`docker run -v` mangles paths in Git Bash** — MSYS rewrites `/w`-style paths. Prefix with
  `MSYS_NO_PATHCONV=1` (all commands above do).
- **clang-format lint fails in CI but "looks fine" locally** — your local formatter is not 14.0.6.
  Only format via the §2 container.
- **`ctest` green but a cluster test flakes under mutation runs** — you built incrementally on the
  bind mount; see the §2 warning.
- **Scripts fail with `\r: command not found`** — CRLF checkout. `.gitattributes` forces LF for
  `*.sh`/`*.yml`/Dockerfiles; re-checkout the file (`git checkout -- <file>`).
- **A fresh cluster is required** whenever the ring hash or clock wire format changed (Tier 4.4/4.5
  are both live): `docker compose down -v`, then `up --build`. A mixed-build cluster splits key
  placement.
- **Node data does not survive `down -v`** — by design for now (constraints §2.12).
