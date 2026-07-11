# Tier 1D — Testing & CI (decisions log)

## What was built

A layered test suite completed to the top of the pyramid, and a GitHub Actions
pipeline that runs the whole thing on every push and pull request.

### Test pyramid
- **Unit** — C++ GoogleTest (ring/hashing, vector-clock compare, quorum arithmetic,
  base64, versioned-value/tombstone, RocksDB) already landed in 1A; Java JUnit 5 +
  Mockito covers the gateway service layer. Added the three remaining error-path
  branches to `KvServiceTest` (cluster-`ERROR`→502, read-quorum-fail→503,
  delete-quorum-fail→503) so the saga-like mapping is fully exercised.
- **Integration** — `RealClusterIT` (new): a Testcontainers `GenericContainer` runs the
  **actual C++ node image** alongside a real PostgreSQL, and REST Assured drives
  `PUT/GET/DELETE` through the gateway against it. This is the layer that proves the
  gateway speaks the genuine wire protocol (framing, base64, vector-clock tokens); the
  pre-existing `GatewayIntegrationTest` keeps using an in-JVM fake for the fast path.
  Split by Maven phase: Surefire runs unit tests in `test`, Failsafe runs `*IT` in `verify`.
- **End-to-end** — `scripts/e2e.sh` stands up the full `docker compose` stack, writes with
  `W=2`, kills a node and reads with `R=2` (availability under one failure), then restarts
  the node and reads to force read repair, asserting `minidynamo_read_repair_total` rose
  (convergence). One command; nonzero exit on any failed assertion.
- `tests.txt` (manual netcat notes) removed; a **Testing** section in `README.md` documents
  and points at the automated suite.

### CI (`.github/workflows/ci.yml`)
Four parallel jobs on every push/PR: **cpp** (clang-format check + CMake build + ctest),
**gateway** (Spotless check + `mvn verify` = unit + `RealClusterIT`), **images** (both
production Dockerfiles build), and **e2e** (`scripts/e2e.sh`, gated behind cpp+gateway so a
compile break fails cheaply first). Maven and CMake outputs are cached; a concurrency group
cancels superseded runs. A status badge is in the README.

### Formatting gates
- **C++:** a `.clang-format` (Google base, 4-space indent, 100 cols, right-bound pointers)
  matching the existing style; the tree was normalized once so the check is clean.
- **Java:** Spotless with a deliberately **minimal** ruleset (import order, no unused imports,
  trimmed trailing whitespace, newline at EOF).

## Key design choices (and the rejected alternative for each)

1. **Split unit vs integration by Maven phase (Surefire `test` / Failsafe `verify`).** Unit
   tests stay fast and hermetic — a bare `mvn test` needs no Docker — while the container-heavy
   `RealClusterIT` runs under `verify`. *Rejected:* one phase for everything — every local
   `mvn test` would then need Docker and pay container startup, discouraging the quick loop.

2. **A real node container for integration, not only the in-JVM fake.** The fake proves the
   gateway's HTTP contract cheaply, but only the real node proves the gateway's *cluster codec*
   against the true implementation — the exact thing most likely to drift. Testcontainers gives
   a throwaway real node keyed off the built image. *Rejected:* trusting the fake alone — it can
   silently diverge from the C++ framing/base64/clock format and the tests would never notice.

3. **A single-node cluster for the integration test; multi-node for the e2e.** One node (with
   `N=W=R=1`) is enough to exercise the wire protocol deterministically and fast; multi-node
   availability-under-failure is a different concern best proven against the real compose
   topology. *Rejected:* a 3-node Testcontainers network in the IT — more faithful but flakier
   (bootstrap/join timing) and redundant with the e2e that already covers it.

4. **Testcontainers over mocks for integration.** Mocks encode our *assumptions* about Postgres
   and the cluster; a real Postgres (Flyway migrations, JPA `validate`, real SQL) and a real
   node catch schema drift, dialect issues, and protocol mismatches a mock cannot. *Rejected:*
   an H2/in-memory DB + mocked cluster — faster, but it validates the mock, not production.

5. **GitHub Actions, and the Jenkins mapping.** Actions is native to the repo host, free for
   this scale, and declarative in-repo. The concepts map directly to Jenkins: a *workflow* ≈ a
   pipeline, a *job* ≈ a stage/agent, a *step* ≈ a build step, `actions/cache` ≈ the workspace/
   stash cache, and branch protection requiring a green run ≈ a Jenkins "gated" merge. *Rejected:*
   Jenkins here — a server to host and maintain for zero benefit on a portfolio repo.

6. **Run the C++ job inside `debian:bookworm-slim`.** clang-format output is version-sensitive;
   pinning the job to the same image the project builds in means the CI formatter is byte-identical
   to the local one, so lint never fails on a version mismatch. *Rejected:* `apt install
   clang-format` on the bare runner — a different patch version can reformat differently and turn
   the gate red on style noise.

7. **`WITH_PROMETHEUS=OFF` for the test/integration builds; ON only for the image job.** The unit
   suite links `kv_core` (no Prometheus), and the IT tests the protocol, not metrics — so skipping
   the prometheus-cpp FetchContent keeps those jobs fast; the full metrics build is still proven by
   the image job. *Rejected:* building with prometheus everywhere — minutes of FetchContent on the
   hot path of every job for no added coverage there.

## Where this could break under adversarial conditions

- **The e2e is the CI long pole.** `docker compose up --build` compiles the node *with* prometheus
  (its `/metrics` is what the convergence check reads) plus the gateway image, so that job runs well
  past the ~5-minute target even with caching. It is gated behind the fast jobs, but a slow runner
  makes total wall-clock time the e2e's time.
- **`RealClusterIT` needs the node image present.** It is skipped unless `NODE_IMAGE` is set (keeping
  `mvn test` hermetic); if CI ever forgets to build/set it, the test silently no-ops rather than
  failing. The trade favors a fast default local run over a load-bearing env var.
- **Formatter version pinning is load-bearing.** clang-format is pinned via the Debian image, but
  Spotless' behavior is pinned only by its plugin version; a future bump could reformat and require
  a re-`apply`. Both are deliberate, mechanical churn — never behavioral.
- **Branch protection is out-of-band.** Requiring a green run before merge to `main` is a GitHub
  repo-admin setting the workflow can't self-apply (see below); until it's switched on, CI reports
  status but does not *enforce* it.
- **Write availability under node failure is partial, and slow to fail.** Writing this e2e surfaced
  a real system property (not a test bug): a `PUT` to a key whose *primary* owner is the downed node
  is forwarded to that dead primary and returns `502 forward_failed` only after the forward times out
  (~3–9s observed). Keys primaried on a live node still meet `W=2` instantly. So the e2e asserts
  *partial* write availability (≥1 success) plus read availability + convergence — the honest thesis
  for a store without hinted handoff. Hinted handoff (accept the write on a stand-in and hand it off
  later) and faster failure detection are the named future work that would close both gaps.

## Enabling branch protection (manual, one-time)

On GitHub: **Settings → Branches → Add branch ruleset** (or *Branch protection rules*) for `main` →
enable **Require status checks to pass before merging**, and select the CI jobs (`cpp`, `gateway`,
`images`, `e2e`) as required. Optionally require a PR and up-to-date branches. This can only be done
by a repo admin in the GitHub UI (or via the REST API with an admin token), so it is not part of the
committed workflow.

## Verification status (honest)

Since there is no remote yet, each CI job is run locally to prove the pipeline is green-able.

Confirmed:
- **cpp job — green.** In the same `debian:bookworm-slim` image CI uses: `clang-format
  --dry-run -Werror` reports **CLEAN** across `src/` and `tests/` (after the one-time
  normalization), and the suite builds (`-DWITH_PROMETHEUS=OFF`) and passes **37/37**.
- **node image** builds with `--build-arg WITH_PROMETHEUS=OFF` (the fast path the gateway/e2e
  jobs use) as `mini-dynamo-node:ci`.
- **gateway job — green.** `mvn verify` = Spotless (normalized one unused import; `check`
  passes) + Surefire unit **31/31** (incl. the three new `KvService` error-path cases) +
  Failsafe **`RealClusterIT` 2/2** driving the gateway against the real node container →
  BUILD SUCCESS.

- **images — green.** `docker compose up --build` builds all four production images (three
  nodes with prometheus ON + gateway jar).
- **e2e — green.** `scripts/e2e.sh` on the full stack: gateway healthy, baseline `PUT W=2` /
  `GET R=2` round-trip, node2 killed → read still served (**availability under one failure**),
  **5/8** degraded writes succeed (the 3 failures are keys primaried on the downed node — the
  502 `forward_failed` documented above), then node2 restarts and re-reads drive
  `read_repair_total` **0 → 5** (**convergence** — exactly the 5 committed writes node2 missed).
  `E2E PASSED`, exit 0.
