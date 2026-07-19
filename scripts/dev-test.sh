#!/usr/bin/env bash
# dev-test.sh — the full C++ check, CI-identical, one command.
#
#   scripts/dev-test.sh                 # format check + build + all tests
#   FORMAT=fix scripts/dev-test.sh      # apply clang-format-14 to the repo first
#   FILTER='Cluster.*' scripts/dev-test.sh   # run a gtest subset
#
# Everything runs inside debian:bookworm-slim — the exact CI image (g++ 12,
# clang-format 14.0.6, RocksDB, GoogleTest) — so "passes here" means "passes in
# CI". The source is COPIED to the container-native filesystem before building:
# incremental builds on the Windows/OneDrive bind mount silently reuse stale
# objects (docs/runbooks/local-dev.md §2), and a check that can lie is worse
# than none.
set -euo pipefail

cd "$(dirname "$0")/.."
REPO="$(pwd -W 2>/dev/null || pwd)"   # -W: native path under Git Bash
IMG=debian:bookworm-slim
PKGS="g++ cmake make clang-format librocksdb-dev libgtest-dev libspdlog-dev ca-certificates"

if [ "${FORMAT:-check}" = "fix" ]; then
  echo "=== clang-format-14: applying in place ==="
  MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO":/w -w /w "$IMG" bash -c "
    apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends clang-format >/dev/null
    find src tests \\( -name '*.cpp' -o -name '*.h' \\) -print0 | xargs -0 clang-format -i
  "
fi

MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO":/w:ro "$IMG" bash -c "
  set -euo pipefail
  cp -r /w /src && cd /src
  apt-get update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends $PKGS >/dev/null
  echo '=== clang-format-14 check (CI exact) ==='
  find src tests \\( -name '*.cpp' -o -name '*.h' \\) -print0 | xargs -0 clang-format --dry-run -Werror
  echo 'format clean'
  echo '=== configure + build ==='
  cmake -S . -B /tmp/b -DBUILD_TESTING=ON -DWITH_PROMETHEUS=OFF > /tmp/cm.log 2>&1 || { tail -20 /tmp/cm.log; exit 1; }
  cmake --build /tmp/b -j\"\$(nproc)\" > /tmp/build.log 2>&1 || { grep -E 'error:' /tmp/build.log | head -30; exit 1; }
  echo 'build ok'
  echo '=== tests ==='
  if [ -n '${FILTER:-}' ]; then
    /tmp/b/tests/kvstore_tests --gtest_filter='${FILTER:-}'
  else
    ctest --test-dir /tmp/b --output-on-failure
  fi
"
echo "=== dev-test PASSED ==="
