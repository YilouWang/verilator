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
  # A live run refreshes the lock every 5 minutes (heartbeat below), so a lock
  # whose heartbeat is older than LOCK_HB_TTL_S belongs to a dead sandbox.
  # Locks without a heartbeat field come from older runs and use LOCK_TTL_S.
  if [ -f "$JOURNAL/lock.json" ] && [ "${RTLFAST_FORCE_LOCK:-0}" != 1 ]; then
    verdict=$(python3 -c "
import json,time
d=json.load(open('$JOURNAL/lock.json'))
age=int(time.time()-d.get('t',0))
ttl=${RTLFAST_LOCK_HB_TTL_S:-1200} if d.get('hb') else $LOCK_TTL_S
print(('held' if age<ttl else 'stale'), age)" 2>/dev/null || echo "stale 999999")
    if [ "${verdict%% *}" = held ]; then
      echo "rtlfast: LOCKED by another run (last heartbeat ${verdict#* }s ago). Stop this run without changes."
      exit 3
    fi
  fi
  RUN_ID="$(hostname)-$$-$(date +%s)"
  echo "$RUN_ID" > /tmp/rtlfast-run-id
  python3 -c "import json,time;json.dump({'t':time.time(),'hb':1,'run':'$RUN_ID','since':'$(stamp)'},open('$JOURNAL/lock.json','w'))"
  # Heartbeat: lives as long as this sandbox, stops once the lock is released
  # or taken over by another run.
  nohup bash -c "while sleep 300; do python3 -c \"
import json,time,sys
p='$JOURNAL/lock.json'
try: d=json.load(open(p))
except Exception: sys.exit(1)
if d.get('run')!='$RUN_ID': sys.exit(1)
d['t']=time.time(); json.dump(d,open(p,'w'))\" || break; done" > /dev/null 2>&1 &
  echo $! > /tmp/rtlfast-heartbeat.pid
  echo "rtlfast: lock taken ($RUN_ID)"
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
