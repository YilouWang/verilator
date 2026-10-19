#!/bin/bash
# ab.sh <cycles> <rounds> dirA dirB ...: interleaved perf runs, prints medians
C=$1; R=$2; shift 2
for r in $(seq $R); do
  for d in "$@"; do
    s=$(taskset -c 2 $d/obj/Vm --mode perf --cycles $C 2>/dev/null | grep -o "cycles_per_sec=[0-9.]*" | cut -d= -f2)
    echo "$d $s"
  done
done | tee /tmp/ab.out
python3 - <<'PY'
import collections,statistics
d=collections.defaultdict(list)
for l in open('/tmp/ab.out'):
    a,b=l.split(); d[a].append(float(b))
for k,v in d.items(): print(k, 'median %.0f min %.0f max %.0f' % (statistics.median(v), min(v), max(v)))
PY
