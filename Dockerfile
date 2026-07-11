# Debian (glibc) rather than Alpine (musl): librocksdb-dev and its transitive
# compression deps link cleanly here, whereas RocksDB on musl is a known source
# of static-linking pain. The image is a little larger; for a portfolio cluster
# that trade is worth the reliable build.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        git \
        librocksdb-dev \
        libspdlog-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# git: FetchContent clones prometheus-cpp (+ its civetweb submodule) at configure
# time. libspdlog-dev: structured JSON logging. docker build has network, so the
# fetch resolves here even though the runtime image has none.

WORKDIR /app
COPY . .

# Build only the node binary here (BUILD_TESTING=OFF keeps the image build from
# fetching GoogleTest); the test suite runs in CI, not in the runtime image.
# WITH_PROMETHEUS is ON by default, so kvstore gets its /metrics endpoint.
RUN cmake -S . -B build -DBUILD_TESTING=OFF && cmake --build build -j4

# Parent directory for each node's own RocksDB instance (DATA_DIR default is
# /data/<node_id>). Shared-nothing: every container has its own /data.
RUN mkdir -p /data

# TCP protocol port is per-node (NODE_PORT); the Prometheus scrape endpoint is
# METRICS_PORT (default 9100). Both are published by docker-compose.
EXPOSE 9100

CMD ["./build/kvstore"]
