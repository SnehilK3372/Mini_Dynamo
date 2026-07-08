# Mini Dynamo - Enterprise Architecture & Build Plan

A standalone roadmap for evolving Mini Dynamo from a working student distributed key-value store into a system that demonstrates enterprise-grade software engineering across the tools common to most enterprise SWE roles.

This document explains **what to build, how each piece works, which tool to use, and why that tool over the alternatives.** It is meant to be read top to bottom once, then used as a reference while you build.

* * *

## How to read this document

- **Part 1–2** explain the design philosophy and the target architecture end to end. Read these first; everything else assumes them.
    
- **Part 3** is the roadmap at a glance.
    
- **Parts 4–10** are the build tiers, in order. Each tier is independently valuable — you can stop after any tier and still have a coherent system.
    
- **Part 11** is a consolidated tool-decision table you can scan quickly.
    
- **Part 12** maps tiers to milestones and to what each milestone lets you honestly claim.
    

**Current state (starting point).** The repo today has a genuinely working consistent-hashing ring with virtual nodes, a custom TCP networking layer, a coordinator-based PUT that replicates to N owners, a read-from-any-replica GET, and a bootstrap-join membership handshake. It does **not** yet have durable storage, real write acknowledgment (replication is fire-and-forget), tunable quorum, vector clocks, or read repair. It also carries committed build artifacts, some dead code, and a one-line README. Tier 0 and Tier 1A exist to close those gaps before any enterprise tooling is layered on.

* * *

## Part 1 — Design philosophy

### The core principle: separate the planes

Real distributed databases are not one monolithic program. They separate concerns into **planes**, and we mirror that:

- **Data plane** — the C++ cluster. Its only job is storing data, replicating it, and enforcing consistency. It is performance-critical and stays close to the metal (raw TCP, an embedded storage engine). This is the part you already built and the part that proves distributed-systems depth.
    
- **Control / API plane** — a Java (Spring Boot) gateway in front of the cluster. Its job is everything the data plane should *not* be burdened with: a clean external REST API, authentication, request validation, admin/cluster-management endpoints, and storing cluster metadata. This is where most of the "enterprise" tooling lives, and it happens to be written in a language you already know.
    
- **Platform plane** — observability (metrics, logs, traces), CI/CD, containerization, and cloud infrastructure. This is cross-cutting: it supports all layers without being part of the request path.
    

Keeping these separate is the single most important architectural decision in this plan. It is why the system will read as "engineered" rather than "a big program with features bolted on," and the separation itself is a strong interview talking point.

### The consistency stance: AP with tunable consistency

The **[CAP Theorem](https://en.wikipedia.org/wiki/CAP_theorem)** states that **any distributed data store can provide at most two of three guarantees: Consistency (all nodes see the same data), Availability (every request gets a response), and Partition Tolerance (the system continues working despite network failures)**

Mini Dynamo is, like the paper it's based on, an **AP** system under the CAP theorem: when the network partitions, it chooses **A**vailability and **P**artition-tolerance over strict **C**onsistency. It stays writeable during failures and converges afterward.

This is a deliberate choice, and knowing *why* matters:

- **CP systems** (backed by consensus protocols like Raft or Paxos — e.g. etcd, ZooKeeper, or the metadata layer of many databases) guarantee that every read sees the latest write (linearizability). The price is that during a partition, the minority side must stop accepting writes to avoid divergence. They trade availability for consistency.
    
- **AP systems** (Dynamo, Cassandra, Riak) never refuse a write if any replica is reachable. The price is that reads can be stale and concurrent writes can conflict. They trade consistency for availability, and then invest in machinery — quorums, vector clocks, read repair, anti-entropy — to make the eventual consistency *manageable*.
    

We build the AP machinery (Tier 1A). The tunability comes from the quorum knobs N, W, and R, which let a client dial anywhere between "fast but possibly stale" and "strongly consistent" per request.

* * *

## Part 2 — Target architecture and end-to-end request flow

The finished system has four layers. A request travels down through them and a response back up.

```
REST client
   │  HTTPS  (PUT /kv/{key}, GET /kv/{key})
   ▼
Load balancer (AWS ALB)
   ▼
Spring Boot API gateway (Java)         ── PostgreSQL (cluster metadata, via JPA)
   │  auth · validation · metrics · trace span
   │  gRPC / TCP
   ▼
Mini Dynamo cluster (C++)              ── coordinator picks N owners on the hash ring
   │  versioned writes (vector clocks) · quorum (W acks / R responses) · read repair
   ▼
Per-node storage: RocksDB (embedded LSM engine, durable)

Cross-cutting platform:
   Prometheus + Grafana (metrics) · ELK (logs) · Jaeger/Tempo (traces)
   GitHub Actions (CI/CD) · Docker/Compose · AWS + Terraform (infra)
```

### A write, end to end

1. Client sends `PUT /kv/user123` with a JSON body and optional `N`, `W`, `R` parameters. It hits the load balancer, which forwards to a gateway instance.
    
2. The gateway authenticates the request (JWT), validates the body against a DTO schema, records a metric (request count, latency timer), and opens a distributed-tracing span.
    
3. The gateway forwards the write to the cluster. Whichever node receives it becomes the **coordinator** for this request.
    
4. The coordinator hashes the key onto the consistent-hashing ring, walks clockwise to find the **N** distinct physical nodes that own it, attaches an updated **vector clock** to the value, and sends the versioned write to all N replicas in parallel.
    
5. Each replica persists the value+clock durably to **RocksDB**, emits its own metrics, and writes a structured log line (shipped to Elasticsearch).
    
6. The coordinator waits until **W** replicas acknowledge, then returns success. The trace span closes; the gateway returns `200`.
    

### A read, end to end

1. Client sends `GET /kv/user123?R=2`. Same path to a coordinator.
    
2. The coordinator queries the N replicas and waits for **R** responses, each carrying a value and its vector clock.
    
3. The coordinator compares the clocks:
    
    - If one version **dominates** all others (its clock is a causal descendant of theirs), that's the current value — it's returned, and any replica that returned a stale version is **repaired** asynchronously (read repair).
        
    - If two or more versions are **concurrent** (neither dominates the other — a genuine conflict from writes during a partition), all conflicting versions ("siblings") are returned for the client or gateway to reconcile.
        
4. Metrics, logs, and the trace record the whole thing; Grafana, Kibana, and Jaeger let you see it after the fact.
    

Everything below is how you get from today's code to this picture, one tier at a time.

* * *

## Part 3 — The build tiers at a glance

| 
Tier

 | 

Theme

 | 

What you add

 | 

Primary payoff

 |
| --- | --- | --- | --- |
| 

0

 | 

Foundation & hygiene

 | 

`.gitignore`, artifact purge, dead-code removal, real README, storage interface

 | 

Repo reads as professional; clean base to build on

 |
| 

1A

 | 

Complete the distributed core

 | 

Durable storage (RocksDB), real quorum (W/R), vector clocks, read repair

 | 

Makes the "Dynamo" claim true; best interview material

 |
| 

1B

 | 

Service & data layer

 | 

Spring Boot gateway, REST API, PostgreSQL metadata

 | 

Java/Spring, REST design, SQL — the enterprise backbone

 |
| 

1C

 | 

Observability

 | 

Prometheus + Grafana (metrics), ELK (logs)

 | 

Metrics + logs + Elasticsearch; demoable dashboards, no frontend

 |
| 

1D

 | 

Testing & CI/CD

 | 

GoogleTest, JUnit/Mockito, Testcontainers, GitHub Actions

 | 

Full test pyramid + automated pipeline

 |
| 

2

 | 

Differentiators

 | 

gRPC, distributed tracing, load + chaos testing, AWS deploy

 | 

Modern RPC, tracing, real benchmarks, a live system

 |
| 

3

 | 

Exceptional

 | 

Kubernetes (EKS), Terraform, SLOs, hinted handoff + Merkle anti-entropy

 | 

Senior-level ops maturity; the complete Dynamo story

 |

* * *

## Part 4 — Tier 0: Foundation and cleanup

Do this first. It's low-effort and it changes how the repo reads from "student assignment" to "someone who knows how to keep a codebase."

### 4.1 Repository hygiene

**Add a** `.gitignore` **and purge build artifacts.** The repo currently commits the entire `build/` tree, `CMakeFiles/`, the compiled `kvstore` binary, `a.out` files, and `.o` objects. Committing build output is the first thing an experienced engineer notices, and it signals unfamiliarity with version control. Add a `.gitignore` covering `build/`, `*.o`, `*.out`, and the binary, and remove those files from history.

**Rename the top-level directory.** It is currently named `Project: Mini-Dynamo` — a colon (illegal in Windows paths) and a space (breaks shell scripts). Rename to something like `mini-dynamo`.

### 4.2 Remove dead code

Several code paths are never executed and should be deleted so the codebase reflects what actually runs:

- `Node::start()` and `Node::server_loop()` — `main.cpp` wires up the TCP server and does its own inline JOIN, so these methods (and their `SEED_HOST`/`SEED_PORT` env path) are never called. `server_loop` is empty anyway.
    
- The second, unused store in `node.h` (`store` / `store_mtx`) — only `local_storage` / `storage_mutex` is used.
    
- `hash.cpp`'s `hashKey` — the router hashes via `hash64` in `util.h`; the FNV function is never called (and its offset basis isn't the standard FNV value anyway).
    
- `config/node_config.json` and the `nlohmann/json` include — nodes get their identity from environment variables, and nothing reads this config file, so the JSON dependency the Dockerfile installs is unused.
    

Deleting dead code is not busywork: a reviewer who sees two storage members and two hash functions can't tell which is real, and that ambiguity itself reads as sloppiness.

### 4.3 Introduce a storage interface (the one piece of new design)

Before Tier 1A adds RocksDB, define a small **storage interface** — an abstract `StorageEngine` with `put(key, versionedValue)`, `get(key)`, and iteration — and make the current in-memory map implement it. This is the "program to an interface, not an implementation" principle. It costs almost nothing now and it means swapping the in-memory map for RocksDB in Tier 1A is a one-file change rather than surgery scattered across `node.cpp`. It also makes the node testable without a real database (you can inject an in-memory fake).

### 4.4 Write a real README

The README is currently one line. Write one that describes what the system *actually does today* (be honest — see the note in Part 12). At minimum: what it is, the architecture in a paragraph, how to build and run the cluster, and the wire protocol. This README grows with each tier.

* * *

## Part 5 — Tier 1A: Complete the distributed core

This is the most important tier and the one that produces the best interview material. It turns Mini Dynamo from "a sharded, replicated store" into a genuine tunable-consistency, conflict-aware Dynamo. Do it before any enterprise tooling — there is no point wrapping a beautiful gateway around a core that silently drops writes.

### 5.1 Durable storage with RocksDB

**What it does.** RocksDB replaces the in-memory `std::unordered_map` behind the storage interface from Tier 0. It is an embedded, persistent key-value store: data survives restarts, which is table stakes for anything calling itself a database.

**How it works.** RocksDB is an **LSM-tree** (log-structured merge-tree) store. Writes go first to an in-memory table (the memtable) and an append-only write-ahead log on disk (for durability), then are flushed in sorted batches to immutable on-disk files (SSTables), which a background process periodically compacts. This design makes writes cheap (sequential appends, no in-place updates) — which is exactly right for an always-writeable store that must never refuse a write.

**Why RocksDB, and why not the alternatives.**

- *Why not keep it in-memory:* no durability. A restart wipes the store; that's not a database.
    
- *Why not PostgreSQL per node:* Postgres is a full relational server — a separate process, a query planner, ACID transactions, SQL. That's enormous overhead for a node whose entire job is `get`/`put` on a byte string. It's also a B-tree (read-optimized, in-place updates), which is the wrong shape for a write-heavy replicated store. Postgres has a role in this system, but as the *metadata* store in the control plane, not as the per-node engine.
    
- *Why not LevelDB:* RocksDB is a fork of LevelDB, hardened for production — better concurrency, column families, far more tuning. It is the engine that real systems (e.g. the storage layer under many databases) actually use. LevelDB is the toy version of the same idea.
    
- *Why not roll your own WAL + files:* you'd be reimplementing, worse, exactly what RocksDB gives you for free (durability, crash recovery, compaction). The engineering signal is choosing the right library, not rebuilding it.
    

**How it fits.** Each node owns its *own* RocksDB instance on its *own* disk — never a shared database. This preserves Dynamo's shared-nothing model: nodes coordinate only over the network, never through shared storage.

### 5.2 Versioned values and vector clocks

**What a vector clock is.** A vector clock is a small map from node ID to a counter: `{node1: 3, node2: 1}`. It records "how many writes from each node this version has causally seen." Every value you store becomes a **versioned value**: the data plus its vector clock. This is the change that makes conflict *detection* possible — today the code stores a bare string and blindly overwrites.

**How it works.**

- When a node coordinates a write, it increments its own entry in the clock and stores value+clock.
    
- Given two versions A and B, you compare their clocks:
    
    - **A dominates B** (A is a causal descendant — it "happened after" and has seen everything B saw) if every counter in A is ≥ the matching counter in B, and at least one is strictly greater. Then A supersedes B; B can be discarded.
        
    - **A and B are concurrent** (a real conflict) if neither dominates — A has a higher counter for some node *and* B has a higher counter for some other node. This happens when two clients wrote to different replicas during a partition. Neither is "newer"; both are kept as **siblings**.
        
- On a read, siblings are surfaced so they can be reconciled (by the client, or by an application-specific merge — e.g. union two shopping carts).
    

**Why vector clocks, and why not the alternatives.**

- *Why not last-write-wins with wall-clock timestamps:* it's simpler, but wall clocks on different machines drift and skew, so "latest timestamp" is not a reliable ordering — and worse, LWW *silently discards* one of two concurrent writes. It can't even tell a conflict happened; it just loses data. (Notably, Cassandra chose LWW-timestamps for operational simplicity — a legitimate tradeoff you should be able to discuss — but it accepts that data loss.)
    
- *Why not CRDTs (conflict-free replicated data types):* CRDTs merge automatically without ever producing a conflict, which is more powerful — but they constrain your data model to specific mergeable types (counters, grow-only sets, etc.). For a general-purpose key-value store holding opaque values, that's the wrong tool. Vector clocks are the right level of abstraction and they're what the Dynamo paper uses.
    

**How it fits.** This touches the message format (add a clock field), the storage value type (value+clock), and the coordinator logic on both write and read. It's the intellectual heart of the project.

### 5.3 Tunable quorum: real N, W, and R

**What it does.** This replaces today's fire-and-forget replication (which returns `OK` even if every replica write failed) with a real coordinator quorum, and adds the client-facing consistency knob.

**How it works.** Three numbers govern each request:

- **N** — the replication factor: how many replicas store each key (found by walking the hash ring).
    
- **W** — the write quorum: the coordinator sends the write to all N replicas but returns success only after **W** of them acknowledge. (Today, nothing waits for any acknowledgment — the fix is to make replication a request/response that counts acks, instead of `send`\-and-close.)
    
- **R** — the read quorum: the coordinator queries replicas and answers after **R** of them respond.
    

The relationship between them is the consistency dial:

- **W + R > N** guarantees the write set and read set overlap in at least one replica, so a read is guaranteed to see the latest completed write — effectively strong consistency. Example: N=3, W=2, R=2.
    
- **W + R ≤ N** gives up that guarantee for lower latency and higher availability — reads may be stale, but you tolerate more nodes being down. Example: N=3, W=1, R=1 (fast, weakly consistent).
    

Exposing N/W/R per request is precisely the "tunable consistency" the system advertises, and letting a client choose per-operation is the whole point of the Dynamo design.

**Why quorum, and why not consensus.** The obvious alternative is a consensus protocol (Raft or Paxos), which gives linearizable, always-correct reads. But consensus requires a leader and a majority to make progress, so under a partition the minority side stops serving — that's a CP system, which sacrifices the availability that is Mini Dynamo's entire reason for existing. Leaderless quorums keep the system available (in the extreme, W=1 means "accept the write if a single replica is up") and push the cost onto eventual convergence, which the next section and Tier 3 handle. Choosing quorum over consensus *is* choosing AP over CP, deliberately.

### 5.4 Read repair

**What it does.** Read repair is the cheapest convergence mechanism: it heals stale replicas as a side effect of normal reads. When a read gathers R responses and finds that some replicas returned an older version than others, the coordinator asynchronously pushes the current version to the stale ones — after it has already answered the client, so it adds no latency to the read.

**How it works.** It reuses the vector-clock comparison from 5.2. Among the R responses, the coordinator identifies the dominant version, returns it, and then fires background writes to any replica whose version it dominated. Over time, frequently-read keys stay consistent for free.

**Why read repair first, and what comes later.** It's the natural first convergence mechanism because it rides on the read path you already have and requires no background machinery. But it only heals keys that are *read* — a key that's written and never read can stay divergent on a stale replica forever. Two further mechanisms (deferred to Tier 3, because they're where the "exceptional" signal lives) complete the story:

- **Hinted handoff** — if a replica is down at *write* time, the coordinator writes to the next available node with a "hint" recording where it really belongs, and delivers it when the target recovers. This is what makes the system "always writeable" even when a replica is down.
    
- **Anti-entropy with Merkle trees** — a periodic background process where replicas compare compact hash trees of their data to find and repair divergence efficiently, catching the rarely-read keys read repair misses.
    

Building read repair now and naming these two as the roadmap shows you understand the full convergence design, not just the easy part.

* * *

## Part 6 — Tier 1B: The service and data layer

With a sound core, you now add the enterprise-facing layers. This is where most of the "commonly used tools" skills come from.

### 6.1 The Spring Boot API gateway

**What it does.** A Java service that sits in front of the C++ cluster and is the only thing clients talk to. It exposes a clean REST API, authenticates and validates requests, offers admin endpoints (cluster status, node list, ring inspection), and owns the metadata database. It translates REST calls into cluster-protocol calls.

**How it works — layered architecture.** Structure it the standard enterprise way, because *that structure is itself the skill*:

- **Controller layer** — handles HTTP, maps routes to methods, deals in DTOs (data transfer objects), returns proper status codes. No business logic.
    
- **Service layer** — the business logic: talk to the cluster, apply quorum defaults, assemble responses, handle conflicts.
    
- **Repository layer** — data access to PostgreSQL via Spring Data JPA.
    
- **DTOs vs entities** — never expose JPA entities directly through the API; map to DTOs. This keeps your database schema and your API contract independent, so you can change one without breaking the other.
    

Layer in the Spring ecosystem pieces, each of which is a recognizable enterprise skill:

- **Spring Web** — the REST framework.
    
- **Spring Data JPA** — database access without hand-written SQL boilerplate (though you'll still write queries).
    
- **Bean Validation** (`@Valid`, `@NotNull`, etc.) — declarative request validation.
    
- **Spring Security** — JWT-based auth on the API.
    
- **Spring Boot Actuator** — production endpoints (`/health`, `/metrics`) that feed the observability layer.
    
- **springdoc-openapi** — auto-generates an interactive Swagger UI from your annotations, giving you live API documentation for free (and, usefully, requiring no frontend work).
    

**REST API design.** Design the contract deliberately — this is a named skill in nearly every backend posting:

- `PUT /kv/{key}` to write, `GET /kv/{key}` to read, `DELETE /kv/{key}` to delete.
    
- Admin: `GET /cluster/nodes`, `GET /cluster/ring`, `GET /health`.
    
- Use correct status codes (`200`, `201`, `400` for validation failures, `404`, `409`/`300` to surface conflicting siblings, `503` when a quorum can't be met).
    
- Version the API (`/v1/...`) so you can evolve it.
    
- Make writes idempotent where possible.
    

**Why Java + Spring Boot, and why not the alternatives.**

- *Why Java + Spring Boot:* it's the enterprise default, it's a language you already know (low risk under time pressure), and Spring Boot is "batteries included" — REST, dependency injection, JPA, validation, security, and metrics all integrate out of the box. It is the single most recognizable enterprise-backend skill you can demonstrate.
    
- *Why not Go:* Go is arguably the more idiomatic language for a lightweight networking gateway (smaller footprint, excellent concurrency), and it's worth learning eventually. But you don't know it, and Spring demonstrates the enterprise-framework competency more directly. If you *want* to learn Go, this gateway is the natural place — just accept the added risk.
    
- *Why not Node.js/Express:* also fine technically, but a weaker "enterprise backend" signal than a JVM/Spring stack, and again a language you'd be learning from scratch.
    

**Architectural honesty (a good interview point).** Putting a JVM gateway in front of a C++ performance cluster adds a network hop and a process. In a latency-critical system you might expose the cluster's protocol more directly. It's justified here as a client-facing API gateway — the exact pattern real distributed databases use to front their storage nodes — and being able to articulate *why you'd make a different choice under different constraints* is precisely the judgment enterprise interviews probe.

### 6.2 The gateway-to-cluster protocol

The gateway has to speak to the cluster. There are three options, and the right answer changes by tier:

- **Now (Tier 1B):** have the gateway speak the cluster's existing pipe-delimited TCP protocol. It's the least new work and gets the whole stack running.
    
- **Later (Tier 2):** migrate the boundary to **gRPC + Protocol Buffers**. See 9.1 for the full rationale — in short, gRPC gives a typed schema, efficient binary framing, and code generation for both C++ and Java, which is the enterprise standard for internal service-to-service RPC and a far more robust boundary than an ad-hoc string protocol.
    
- *Why not REST between gateway and cluster:* JSON-over-HTTP is heavier and untyped for internal calls; REST is the right choice for the *external* client API, gRPC for the *internal* service boundary.
    

Note that **inter-node** communication (node-to-node replication) can stay raw TCP — that low-level socket code is a genuine highlight, and it's the data plane's internal business.

### 6.3 PostgreSQL for control-plane metadata

**What it does.** A relational database holding the cluster's *metadata* (not its data): the node registry (which nodes exist, their addresses), ring/topology snapshots, configuration versions, and an audit log of administrative operations. The gateway reads and writes it via JPA.

**How it works.** This is naturally relational, low-volume, and benefits from ACID transactions and ad-hoc SQL queries — exactly what a relational database is for. It's also where you demonstrate SQL and schema design, and it's the reason to use Spring Data JPA at all.

**Why PostgreSQL, and why not the alternatives.**

- *Why Postgres:* it's the leading open-source relational database, it integrates cleanly with Spring Data JPA, and it's a near-universal enterprise skill. (MySQL and Oracle appear in enterprise stacks too; Postgres is the strongest single choice and the concepts transfer.)
    
- *Why not put metadata in RocksDB too:* you'd lose relational modeling, SQL, and the JPA demonstration — and metadata is genuinely relational (nodes, configs, audit entries with relationships and queries), so a relational database is the correct fit. Using RocksDB for opaque data and Postgres for structured metadata is the right separation: each store does what it's good at.
    

* * *

## Part 7 — Tier 1C: Observability

Observability is what separates a demo from something that looks operable. It also solves your frontend gap: **both pieces below are configured, not coded — no React, no JavaScript.** There are three pillars (metrics, logs, traces); this tier does metrics and logs, and Tier 2 adds traces.

### 7.1 Metrics: Prometheus + Grafana

**What they do.** **Prometheus** collects numeric time-series metrics (request rate, latency percentiles, quorum success rate, read-repair count, per-node health). **Grafana** turns those into dashboards.

**How it works.** Prometheus uses a **pull** model: it periodically scrapes an HTTP `/metrics` endpoint that each service exposes. In the Java gateway, **Micrometer** (which Spring Boot Actuator uses) exposes that endpoint automatically. In the C++ nodes, the **prometheus-cpp** library does the same. Grafana queries Prometheus with **PromQL** and renders panels. You build a dashboard once (picking panels, writing queries) — there's no code and no build step, and the result is genuinely impressive to demo: watch p99 latency and quorum health move in real time while you kill a node.

**Why this stack, and why not the alternatives.**

- *Why Prometheus + Grafana:* it's open-source and self-hostable (free for a portfolio project), it's the de facto standard for infrastructure metrics, and the pull model suits a dynamic cluster. Grafana is the standard visualization layer on top.
    
- *Why not Datadog:* Datadog is excellent but it's a paid SaaS you can't self-host, so it's unsuitable for a portfolio project — though it's worth being aware of, since many companies use it and it appears in enterprise stacks.
    
- *Why not use the logging stack (ELK) for metrics:* Elasticsearch is optimized for full-text search over documents, not for high-cardinality numeric time series. You *can* store metrics in it, but it's the wrong tool; Prometheus is purpose-built. Using the right store for each signal is the point.
    

### 7.2 Logs: the ELK stack

**What it does.** Centralized logging: every service's log lines are collected in one place, indexed, searchable, and visualizable. **E**lasticsearch stores and indexes; **L**ogstash (or Fluentd) collects and parses; **K**ibana searches and visualizes.

**How it works.** Each service emits **structured (JSON) logs** — Logback/SLF4J in Java, spdlog in C++ — with fields like node ID, request ID, operation, and outcome. A shipper (**Logstash** or **Fluentd**) collects those lines, parses them, and forwards them to **Elasticsearch**, which indexes them for fast search. **Kibana** is the UI where you query and build log dashboards. A great demo: filter to `operation:read_repair` and watch repair events spike right after you restart a downed node.

**Why ELK, and why not the alternatives.**

- *Why ELK:* it's the classic centralized-logging and log-analytics stack, and it teaches you **Elasticsearch** — a high-value, widely-used tool that also happens to be the "search engine" skill (the same engine powers product search in e-commerce). Learning it here pays off well beyond this project. Both Logstash and Fluentd are common shippers; Logstash is the canonical ELK pairing, Fluentd is the lighter CNCF alternative — either is fine.
    
- *Why not Grafana Loki:* Loki is a leaner, cheaper modern log aggregator that indexes only labels (not full text) and pairs neatly with the Grafana you're already running — architecturally it's arguably the *cleaner* choice for a small system. The reason to choose ELK anyway is learning value: Elasticsearch is the more broadly useful skill and appears far more often in enterprise stacks. This is a deliberate trade of operational leanness for skill breadth, and being able to explain that trade is itself worth points.
    

**A note on weight.** Running Prometheus + Grafana *and* full ELK is heavy for a portfolio project (Elasticsearch is memory-hungry). That's an acceptable, deliberate choice here to touch both the metrics and log paradigms — but in a lean production system you might consolidate (for example, Prometheus + Grafana + Loki, or a single SaaS like Datadog). Stating that tradeoff out loud is the senior signal; don't pretend the maximal stack is free.

* * *

## Part 8 — Tier 1D: Testing and CI/CD

### 8.1 The test pyramid

Build tests at three levels, from many-and-fast to few-and-thorough. "Full software testing life cycle" is a named requirement in most enterprise postings, and a real suite is also the "code quality" signal.

- **Unit tests (many, fast).** Test pure logic in isolation.
    
    - C++ core with **GoogleTest**: ring/hashing behavior, vector-clock comparison (dominance vs concurrency — the highest-value tests, since that logic is subtle), quorum arithmetic.
        
    - Java gateway with **JUnit 5 + Mockito**: service-layer logic with the cluster mocked out.
        
    - *Why GoogleTest:* the de facto C++ test framework, good fixtures and matchers. (**Catch2** is a fine header-only alternative.) *Why JUnit + Mockito:* the Java standard.
        
- **Integration tests (fewer, slower).** Test components against real dependencies.
    
    - **Testcontainers** spins up a real PostgreSQL (and a real cluster) inside Docker during the test run, so you test against the actual database, not a mock. This high-fidelity approach is a strong modern-testing signal.
        
    - **REST Assured** exercises the gateway's HTTP API end to end.
        
    - *Why Testcontainers:* testing against real services catches integration bugs that mocks hide, and it's a recognizably professional practice.
        
- **End-to-end tests (few).** One test that stands up the whole stack, writes with W=2, kills a node, reads, and asserts the system stayed available and converged — this validates the project's actual thesis.
    

Replace the current `tests.txt` (six manual `netcat` commands) with this suite.

### 8.2 CI/CD with GitHub Actions

**What it does.** On every push, automatically lint, build both the C++ and Java components, run the unit and integration tests, build the Docker images, and (in Tier 2) deploy.

**How it works.** A YAML workflow defines stages: format check (clang-format / Checkstyle) → build (CMake + Maven) → unit tests → integration tests (Testcontainers) → build and scan Docker images (**Trivy** for vulnerability scanning) → publish coverage (**Codecov**, with a README badge). A red build blocks the merge — this is what "ensure code quality through reviews and testing" looks like in practice.

**Why GitHub Actions, and why not the alternatives.**

- *Why GitHub Actions:* your code is on GitHub, so there's zero CI infrastructure to run; pipelines are YAML in the repo; the action marketplace is huge; it's free for public repos.
    
- *Why not Jenkins:* Jenkins is self-hosted and needs a server to run, and it's older — but it appears in many enterprise stacks (including Fast Retailing's), and the concepts are identical (declarative pipeline, build/test/deploy stages, artifacts). Build with Actions and be able to say "same model as Jenkins." If you want to have literally touched it, running a Jenkins container and porting one pipeline is a half-day exercise.
    
- *Why not GitLab CI:* solid, but tied to GitLab; Actions is the pragmatic choice given GitHub hosting.
    

Work in a **pull-request-based Git workflow** (feature branches, PRs, branch protection) so the *process* — not just the code — demonstrates the collaboration model enterprises use.

* * *

## Part 9 — Tier 2: Differentiators

These make the project stand out from the field. Any one is a strong interview story.

### 9.1 gRPC + Protocol Buffers for the service boundary

**What it does.** Replaces the ad-hoc pipe-delimited protocol on the gateway-to-cluster boundary with a typed, schema-defined RPC layer.

**How it works.** You define the service and message types in a `.proto` schema. Protocol Buffers generates strongly-typed client and server code for both C++ and Java from that one schema, and messages travel as compact binary over HTTP/2 (with multiplexing and streaming).

**Why gRPC, and why not the alternatives.** *Why gRPC:* a single source-of-truth schema, efficient binary framing, and generated code in both languages — it's the enterprise standard for internal service-to-service communication and a far more robust boundary than parsing strings. *Why not REST/JSON internally:* heavier and untyped; REST is for the external client API. *Why not keep the string protocol:* it has no schema and fragile framing — fine for a student demo, not "enterprise." (Note: adding gRPC to a C++ service is non-trivial, which is exactly why it's a Tier 2 differentiator rather than a Tier 1 requirement.)

### 9.2 Distributed tracing: OpenTelemetry + Jaeger

**What it does.** Lets you *see* a single request's path across every service and replica — the gateway span, the coordinator span, and each replica write — as one connected timeline with per-hop latency.

**How it works.** **OpenTelemetry** is a vendor-neutral instrumentation standard: you add spans in code, and a trace ID propagates across the gateway-to-cluster-to-replica calls. **Jaeger** (or **Grafana Tempo**) stores and displays the assembled traces.

**Why this, and why not the alternatives.** *Why OpenTelemetry:* it's the CNCF standard and vendor-neutral, so you instrument once and can export anywhere (no lock-in). *Why Jaeger:* mature, popular, easy to run. *Why Tempo instead:* it integrates with the Grafana you already run, giving a single pane of glass — a reasonable alternative. *Why not Zipkin:* older; OTel + Jaeger is the current default. Seeing a request fan out across coordinator and replicas is a genuinely impressive, uncommon demo.

### 9.3 Load testing and chaos testing

**What it does.** Produces real numbers and empirically validates the availability thesis, instead of just describing it.

**How it works.**

- **Load testing** with **k6**: scripts (written in JavaScript) drive concurrent traffic at the API and report throughput and latency distributions. *Why k6:* modern, developer-friendly, scriptable. (*Why not JMeter:* heavier and GUI-oriented; *why not Locust:* Python-based, a fine alternative.)
    
- **Chaos testing**: a script (or a tool like **Toxiproxy**/**Pumba** for network fault injection) kills nodes or injects latency *during* a load test, and you assert the system stays available and converges afterward. This is the empirical proof of the AP design.
    

Write the results up as a short **benchmark report** (throughput, p50/p99 latency, behavior through N node failures) in the repo. Concrete numbers turn "fault-tolerant" from a claim into a demonstrated fact.

### 9.4 AWS deployment

**What it does.** Puts the whole stack on real cloud infrastructure with a public endpoint, so it's a live system rather than a localhost demo.

**How it works.** **ECR** stores the container images; **ECS Fargate** runs them without you managing servers; **RDS** provides a managed PostgreSQL; an **Application Load Balancer** fronts the gateway. The CD stage of GitHub Actions deploys on merge to main.

**Why these choices, and why not the alternatives.** *Why AWS:* the market leader and the safest generalist cloud skill. *Why ECS Fargate first (rather than EKS/Kubernetes):* Fargate is serverless containers — no cluster to manage — so it's the shortest path to "it's deployed"; Kubernetes is the Tier 3 upgrade. *Why RDS rather than self-managed Postgres:* managed means backups, patching, and failover are handled, so you spend effort on the system, not on database ops. *Why not GCP/Azure:* AWS is the strongest single choice for breadth of opportunity; be aware the others exist (Fast Retailing's data teams, for instance, use GCP).

* * *

## Part 10 — Tier 3: Exceptional

Senior-level signals. Optional, but each substantially raises the ceiling.

### 10.1 Kubernetes on EKS

**What it does.** Orchestrates the containers with production-grade scheduling, self-healing, and scaling.

**How it works.** The C++ nodes deploy as a **StatefulSet** (which gives each replica a stable identity and its own persistent volume — correct for stateful storage nodes), the gateway as a **Deployment**, with **Services** for networking, **ConfigMaps/Secrets** for configuration, **liveness/readiness probes** so Kubernetes knows when a node is healthy, and a **Horizontal Pod Autoscaler** to scale the gateway under load. **Helm** packages it all.

**Why Kubernetes, and why StatefulSet specifically.** *Why K8s:* it's the enterprise container-orchestration standard, and running a stateful distributed system on it demonstrates real ops maturity. *Why a StatefulSet, not a Deployment, for the nodes:* storage nodes have identity and durable disks; a StatefulSet preserves stable network identities and binds each pod to its own persistent volume, which a stateless Deployment can't. *Why not just Docker Compose:* Compose is great for local dev but doesn't do cross-machine scheduling, self-healing, or autoscaling. *Why not K8s from day one:* it's heavy and steep — pointless until the system works end to end.

### 10.2 Terraform (infrastructure as code)

**What it does.** Defines all the AWS infrastructure declaratively in version-controlled files, so the environment is reproducible instead of hand-clicked in a console.

**Why Terraform, and why not the alternatives.** *Why Terraform:* it's cloud-agnostic, declarative, and the industry-standard IaC tool with a huge provider ecosystem and real state management. *Why not CloudFormation:* AWS-only. *Why not CDK/Pulumi:* those let you write infra in a general-purpose language (nice), but Terraform's declarative HCL is the lingua franca and the most in-demand IaC skill.

### 10.3 SLOs, and completing the convergence story

- **SLOs / SLIs and error budgets.** Define service-level objectives (e.g. "p99 read latency < 50 ms," "99.9% availability"), measure them off your Prometheus metrics, and frame reliability in error-budget terms. This is senior-level operational thinking and ties your whole observability stack together.
    
- **Hinted handoff** and **Merkle-tree anti-entropy** (introduced in 5.4). Building these completes the Dynamo paper's full convergence design — the always-writeable guarantee under replica failure, and efficient background repair of rarely-read data. Together with read repair, they're the complete answer to "how does an AP system stay consistent over time."
    

* * *

## Part 11 — Consolidated tool decision reference

| 
Layer

 | 

Tool

 | 

What it does

 | 

Why this over the alternative

 |
| --- | --- | --- | --- |
| 

Per-node storage

 | 

**RocksDB**

 | 

Embedded, durable LSM key-value engine

 | 

Write-optimized and embedded (shared-nothing); Postgres-per-node is a read-optimized server, far too heavy; LevelDB is the un-hardened version

 |
| 

Versioning

 | 

**Vector clocks**

 | 

Detect causal vs concurrent writes

 | 

LWW-timestamps silently lose concurrent writes and rely on skewed clocks; CRDTs constrain the data model

 |
| 

Consistency

 | 

**Quorum (N/W/R)**

 | 

Tunable per-request consistency

 | 

Consensus (Raft/Paxos) is CP — it sacrifices the availability that is the whole point

 |
| 

Convergence

 | 

**Read repair** (+ hinted handoff, Merkle anti-entropy later)

 | 

Heal stale replicas

 | 

Cheapest first mechanism; rides the existing read path; the others complete the story in Tier 3

 |
| 

API gateway language

 | 

**Java + Spring Boot**

 | 

Client-facing REST/control plane

 | 

You already know Java; enterprise default; batteries-included. Go is more idiomatic but new to you; Node is a weaker enterprise signal

 |
| 

Service boundary

 | 

**gRPC + Protobuf** (Tier 2)

 | 

Typed, efficient internal RPC

 | 

Schema + codegen + binary beats an ad-hoc string protocol; REST is for the external API, not internal calls

 |
| 

Metadata store

 | 

**PostgreSQL**

 | 

Relational cluster metadata + audit

 | 

Metadata is relational and needs SQL/ACID; keeping it in RocksDB loses modeling and the JPA skill

 |
| 

Metrics

 | 

**Prometheus + Grafana**

 | 

Time-series metrics + dashboards

 | 

Open-source, self-hostable, purpose-built for metrics; Datadog is paid SaaS; Elasticsearch is the wrong store for numeric series

 |
| 

Logs

 | 

**ELK (Elasticsearch/Logstash/Kibana)**

 | 

Centralized, searchable logs

 | 

Teaches Elasticsearch (high value, doubles as search); Loki is leaner but a narrower skill

 |
| 

Tracing

 | 

**OpenTelemetry + Jaeger** (Tier 2)

 | 

See a request across services

 | 

Vendor-neutral instrumentation (no lock-in); Jaeger is mature; Zipkin is older

 |
| 

Unit tests

 | 

**GoogleTest** / **JUnit 5 + Mockito**

 | 

Fast isolated logic tests

 | 

Language-standard frameworks

 |
| 

Integration tests

 | 

**Testcontainers** + **REST Assured**

 | 

Real-dependency + API tests

 | 

Real services catch bugs mocks hide

 |
| 

Load / chaos

 | 

**k6** + **Toxiproxy/Pumba**

 | 

Throughput/latency + fault injection

 | 

k6 is modern and scriptable; JMeter is heavy, Locust a fine alternative

 |
| 

CI/CD

 | 

**GitHub Actions**

 | 

Automated build/test/deploy

 | 

Zero infra, GitHub-native, free for public repos; concepts transfer to Jenkins

 |
| 

Local orchestration

 | 

**Docker Compose**

 | 

Multi-container dev stack

 | 

Simplest local orchestration

 |
| 

Prod orchestration

 | 

**Kubernetes (EKS)** (Tier 3)

 | 

Scheduling, self-healing, scaling

 | 

StatefulSets fit stateful nodes; Compose can't schedule/heal/scale across machines

 |
| 

Cloud

 | 

**AWS (ECS Fargate, RDS, ECR, ALB)**

 | 

Hosting

 | 

Market leader; Fargate needs no cluster; RDS offloads DB ops

 |
| 

IaC

 | 

**Terraform** (Tier 3)

 | 

Declarative, reproducible infra

 | 

Cloud-agnostic and the most in-demand IaC tool; CloudFormation is AWS-only

 |

* * *

## Part 12 — Milestones, sequencing, and honest claims

Build in this order; each milestone is independently demoable and independently claimable.

1. **M0 — Clean foundation.** Tier 0 complete: no build artifacts, no dead code, a real README, a storage interface in place. *Claim: professional repository hygiene and a maintainable codebase.*
    
2. **M1 — A real Dynamo.** Tier 1A complete: durable RocksDB storage, real W/R quorum, vector clocks, read repair. *Claim (now true): a tunable-consistency, conflict-aware distributed key-value store with durable per-node storage.*
    
3. **M2 — Enterprise service layer.** Tier 1B complete: Spring Boot gateway, REST API, PostgreSQL metadata. *Claim: Java/Spring Boot backend, REST API design, PostgreSQL/SQL.*
    
4. **M3 — Observable.** Tier 1C complete: Prometheus/Grafana dashboards and ELK log search. *Claim: metrics and centralized logging with Prometheus, Grafana, and Elasticsearch — no frontend required.*
    
5. **M4 — Tested and automated.** Tier 1D complete: full test pyramid and a GitHub Actions pipeline. *Claim: full-lifecycle testing (unit/integration/e2e) and CI/CD.*
    
6. **M5 — Differentiated.** Tier 2 complete: gRPC boundary, distributed tracing, a benchmark report, and a live AWS deployment. *Claim: modern RPC, distributed tracing, load/chaos-tested, deployed to AWS.*
    
7. **M6 — Exceptional.** Tier 3 complete: Kubernetes, Terraform, SLOs, and the full convergence story. *Claim: Kubernetes orchestration, infrastructure as code, SLO-driven reliability.*
    

**On honesty (read this).** Only claim a capability once the milestone that builds it is done. The gap between the aspirational description and the actual code today is exactly the gap these tiers close, in an order where the code backs every word. If a résumé or interview says "vector clocks" or "quorum," an interviewer can and will ask you to walk through that code — so the rule is simple: build it, *then* claim it. Two current phrases to retire until they're earned: "high performance" (needs the Tier 2 benchmarks to be defensible) and "stress-tested" (needs the Tier 2 load tests). After M5, both are true and you can use them freely.