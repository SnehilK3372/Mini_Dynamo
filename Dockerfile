# Debian (glibc) rather than Alpine (musl): librocksdb-dev and its transitive
# compression deps link cleanly here, whereas RocksDB on musl is a known source
# of static-linking pain. The image is a little larger; for a portfolio cluster
# that trade is worth the reliable build.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ \
        cmake \
        make \
        librocksdb-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Build only the node binary here (BUILD_TESTING=OFF keeps the image build from
# fetching GoogleTest); the test suite runs in CI, not in the runtime image.
RUN cmake -S . -B build -DBUILD_TESTING=OFF && cmake --build build -j4

# Parent directory for each node's own RocksDB instance (DATA_DIR default is
# /data/<node_id>). Shared-nothing: every container has its own /data.
RUN mkdir -p /data

CMD ["./build/kvstore"]
