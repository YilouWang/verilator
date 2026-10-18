#!/bin/bash
# Release the run lock at the end of a run (only if this sandbox holds it).
JOURNAL="${RTLFAST_JOURNAL_MEM:-/mnt/memory/rtlfast-journal}"
[ -f /tmp/rtlfast-heartbeat.pid ] && kill "$(cat /tmp/rtlfast-heartbeat.pid)" 2>/dev/null
if [ -f /tmp/rtlfast-run-id ] && ! grep -q "$(cat /tmp/rtlfast-run-id)" "$JOURNAL/lock.json" 2>/dev/null; then
  echo "rtlfast: lock is not ours, leaving it"
  exit 0
fi
rm -f "$JOURNAL/lock.json" && echo "rtlfast: lock released"
