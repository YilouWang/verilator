#!/bin/bash
# Fetch benchmark designs (open source) into bench/<design>/src. Idempotent.
# Source: GSIM ready-to-run repository, pinned commit. Each design comes with
# the same Verilog that GSIM's own Verilator comparison uses, and a Linux image.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CACHE="${RTLFAST_CACHE:-/workspace/cache}"
RTR_URL="https://github.com/jaypiper/gsim-ready-to-run.git"
RTR_SHA="55803f6a0f867e8bad7339f707fd6331d2c41c34"
mkdir -p "$CACHE"
if [ ! -d "$CACHE/rtr/.git" ]; then
  git clone --filter=blob:none --no-checkout "$RTR_URL" "$CACHE/rtr"
fi
git -C "$CACHE/rtr" fetch --depth 1 origin "$RTR_SHA" 2>/dev/null || true
git -C "$CACHE/rtr" checkout -q "$RTR_SHA" -- difftest/TestHarness-rocket.tar.bz2 \
  difftest/TestHarness-SmallBoom.tar.bz2 bin/linux.tar.bz2
get() { # design tarball svname
  local d="$1" tb="$2" sv="$3"
  mkdir -p "$HERE/$d/src"
  if [ ! -s "$HERE/$d/src/$sv" ]; then
    tar -xjf "$CACHE/rtr/difftest/$tb" -C "$HERE/$d/src"
  fi
  if [ ! -s "$HERE/$d/src/linux.bin" ]; then
    tar -xjf "$CACHE/rtr/bin/linux.tar.bz2" -C "$HERE/$d/src"
  fi
  ls -la "$HERE/$d/src"
}
get rocket TestHarness-rocket.tar.bz2 TestHarness-rocket.sv
get smallboom TestHarness-SmallBoom.tar.bz2 TestHarness-SmallBoom.sv
echo "rtlfast: benchmarks ready"
