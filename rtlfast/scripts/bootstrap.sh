#!/bin/bash
# rtlfast run bootstrap. Run at the start of every session:
#   nohup rtlfast/scripts/bootstrap.sh > /workspace/bootstrap.log 2>&1 &
# then poll the log. Exit codes: 0 ready, 3 another run holds the lock.
#
# Steps: take the lock, sync the canonical oracle, build the reference
# Verilator (pinned base commit) and the DUT (this checkout), fetch the
# benchmarks, and re-check HEAD against the gate.
set -euo pipefail
REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
K="$REPO/rtlfast"
ORACLE_MEM="${RTLFAST_ORACLE_MEM:-/mnt/memory/rtlfast-oracle}"
JOURNAL="${RTLFAST_JOURNAL_MEM:-/mnt/memory/rtlfast-journal}"
WORK="${RTLFAST_WORK:-/workspace/rtlfast-work}"
REF="${RTLFAST_REF:-/workspace/verilator-ref}"
LOCK_TTL_S="${RTLFAST_LOCK_TTL_S:-18000}"
JOBS="$(nproc)"
mkdir -p "$WORK"
stamp() { date -u +%Y-%m-%dT%H:%M:%SZ; }
echo "rtlfast: bootstrap start $(stamp) jobs=$JOBS repo=$REPO"

# 1. Lock (one run at a time; the journal store is shared and live).
if [ -d "$JOURNAL" ]; then
  if [ -f "$JOURNAL/lock.json" ] && [ "${RTLFAST_FORCE_LOCK:-0}" != 1 ]; then
    held=$(python3 -c "import json,time;d=json.load(open('$JOURNAL/lock.json'));print(int(time.time()-d.get('t',0)))" 2>/dev/null || echo 999999)
    if [ "$held" -lt "$LOCK_TTL_S" ]; then
      echo "rtlfast: LOCKED by another run (held ${held}s). Stop this run without changes."
      exit 3
    fi
  fi
  python3 -c "import json,time,socket;json.dump({'t':time.time(),'host':socket.gethostname(),'since':'$(stamp)'},open('$JOURNAL/lock.json','w'))"
  echo "rtlfast: lock taken"
else
  echo "rtlfast: WARNING journal store $JOURNAL not mounted, running without lock"
fi

# 2. Oracle sync. The memory store copy is canonical and read-only.
if [ -d "$ORACLE_MEM" ]; then
  changed=0
  for src in "$ORACLE_MEM"/oracle/* "$ORACLE_MEM"/GATE.json "$ORACLE_MEM"/MISSION.md "$ORACLE_MEM"/BASE_COMMIT; do
    [ -f "$src" ] || continue
    rel="${src#$ORACLE_MEM/}"
    dst="$K/$rel"
    if ! cmp -s "$src" "$dst"; then
      changed=1
      echo "rtlfast: oracle sync replaced $rel"
      mkdir -p "$(dirname "$dst")"
      cp "$src" "$dst"
    fi
  done
  [ "$changed" = 1 ] && echo "$(stamp) oracle files resynced from memory store" >> "$K/notes/ORACLE_SYNC.log"
  echo "rtlfast: oracle in sync"
else
  echo "rtlfast: WARNING canonical oracle $ORACLE_MEM not mounted, using repo copy"
fi
BASE="$(cat "$K/BASE_COMMIT")"

# 3. Git identity and history needed for the reference build.
git -C "$REPO" config user.name "rtlfast agent"
git -C "$REPO" config user.email "rtlfast-agent@users.noreply.github.com"
if ! git -C "$REPO" cat-file -e "$BASE^{commit}" 2>/dev/null; then
  git -C "$REPO" fetch --depth 1 https://github.com/verilator/verilator.git "$BASE"
fi

build_verilator() { # dir label
  local d="$1" label="$2"
  if [ -x "$d/bin/verilator_bin" ] && [ "$(git -C "$d" rev-parse HEAD)" = "$(cat "$d/.rtlfast_built" 2>/dev/null)" ]; then
    echo "rtlfast: $label already built"; return
  fi
  echo "rtlfast: building $label $(stamp)"
  ( cd "$d" && autoconf >/dev/null && ./configure >/dev/null && make -C src opt -j"$JOBS" >"$WORK/build-$label.log" 2>&1 ) \
    || { echo "rtlfast: BUILD FAILED $label, see $WORK/build-$label.log"; tail -40 "$WORK/build-$label.log"; return 1; }
  git -C "$d" rev-parse HEAD > "$d/.rtlfast_built"
  echo "rtlfast: built $label $(stamp)"
}

# 4. Reference = stock Verilator at BASE (a separate worktree).
if [ ! -d "$REF" ]; then
  git -C "$REPO" worktree add --detach "$REF" "$BASE"
fi
build_verilator "$REF" ref
# 5. DUT = this checkout.
dut_ok=1
build_verilator "$REPO" dut || dut_ok=0

# 6. Benchmarks.
"$K/bench/fetch.sh"

# 7. Gate on HEAD.
if [ "$dut_ok" = 1 ]; then
  "$K/scripts/gate.sh" check || echo "rtlfast: HEAD FAILS THE GATE. Fixing that is the first task."
else
  echo "rtlfast: HEAD DOES NOT BUILD. Fixing that is the first task."
fi
echo "rtlfast: bootstrap done $(stamp)"
