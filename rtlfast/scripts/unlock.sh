#!/bin/bash
# Release the run lock at the end of a run.
JOURNAL="${RTLFAST_JOURNAL_MEM:-/mnt/memory/rtlfast-journal}"
rm -f "$JOURNAL/lock.json" && echo "rtlfast: lock released"
