#!/usr/bin/env python3
# Development helper: join a perf profile of a normal --rtl-fast build with the
# activity counters of a VERILATOR_RTLFAST_PROFILE build of the same design
# (same partitions and function names).
#   perf_join.py <profile build obj dir> <map> <prof-counters.txt> <perf.data of the normal build> [n]
# Prints the share of time in partition functions spent in runs without any output
# change, and the most expensive functions with their run and change rates.
import collections, glob, re, subprocess, sys
objdir, mapf, cntf, perfdata = sys.argv[1:5]
top = int(sys.argv[5]) if len(sys.argv) > 5 else 15
f2p = collections.defaultdict(set)
name = None
for path in glob.glob(objdir + '/*.cpp'):
    for line in open(path):
        m = re.match(r'VL_ATTR_OPTSIZE void \w+?___(nba_rtlfast_\w+)\(', line)
        if m:
            name = m.group(1)
            continue
        for m in re.finditer(r'__Vrtlfast_prof_runs\[(\d+)U\] = ', line):
            f2p[name].add(int(m.group(1)))
size = {}
for line in open(mapf):
    f = line.split()
    if f[0] == 'P': size[int(f[1])] = int(f[3])
cnt = {'runs': {}, 'any': {}, 'chg': {}}
cycles = 1
for line in open(cntf):
    f = line.split()
    if f[0] == 'cycles': cycles = int(f[1])
    elif f[0] in cnt: cnt[f[0]][int(f[1])] = int(f[2])
perf = (glob.glob('/usr/lib/linux-tools/*/perf') or ['perf'])[0]  # apt-get install linux-tools-generic
rep = subprocess.run([perf, 'report', '-i', perfdata, '--no-children', '--sort', 'symbol'],
                     capture_output=True, text=True).stdout
rows = []
total = wasted = 0.0
for line in rep.splitlines():
    m = re.match(r'\s*([\d.]+)%\s+\[\.\]\s+\w+?___(nba_rtlfast_\w+)\(', line)
    if not m: continue
    pct, fn = float(m.group(1)), m.group(2)
    parts = f2p.get(fn, ())
    runs = sum(cnt['runs'].get(p, 0) for p in parts)
    anyc = sum(cnt['any'].get(p, 0) for p in parts)
    if not runs: continue
    total += pct
    wasted += pct * (1 - anyc / runs)
    rows.append((pct, runs / cycles, anyc / cycles, sum(size.get(p, 0) for p in parts), fn, sorted(parts)[:4]))
print('time in partition functions %.1f%%, in runs without output change ~%.1f%%' % (total, wasted))
for r in sorted(rows, reverse=True)[:top]:
    print('%.2f%% runs/cyc %.2f change/cyc %.2f nodes %d %s parts %s' % r)
