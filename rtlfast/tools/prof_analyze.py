#!/usr/bin/env python3
# Analyze --rtl-fast profile counters: prof_analyze.py <map> <prof-counters.txt>
import sys, collections
mapf, cntf = sys.argv[1], sys.argv[2]
parts = {}
outs = {}
for line in open(mapf):
    f = line.split()
    if f[0] == 'P':
        parts[int(f[1])] = dict(kind=int(f[2]), size=int(f[3]), nst=int(f[4]), nin=int(f[5]), nout=int(f[6]))
    elif f[0] == 'O':
        outs[int(f[1])] = dict(part=int(f[2]), ncons=int(f[3]), name=f[4])
runs = {}; anyc = {}; chg = {}; cycles = 1
for line in open(cntf):
    f = line.split()
    if f[0] == 'cycles': cycles = int(f[1]); continue
    d = {'runs': runs, 'any': anyc, 'chg': chg}[f[0]]
    d[int(f[1])] = int(f[2])
C = cycles
tot_runs = sum(runs.get(p, 0) for p in parts) / C
tot_any = sum(anyc.get(p, 0) for p in parts) / C
work = sum(runs.get(p, 0) * parts[p]['size'] for p in parts) / C
wasted = sum((runs.get(p, 0) - anyc.get(p, 0)) * parts[p]['size'] for p in parts) / C
outcmp = collections.Counter()
for o, d in outs.items(): outcmp[d['part']] += 1
cmp_eval = sum(runs.get(p, 0) * outcmp[p] for p in parts) / C
chg_tot = sum(chg.values()) / C
print(f"parts={len(parts)} size_total={sum(p['size'] for p in parts.values())} runs/cyc={tot_runs:.1f} any/cyc={tot_any:.1f}")
print(f"work nodes/cyc={work:.0f} wasted(no change) nodes/cyc={wasted:.0f}")
print(f"outputs compared/cyc={cmp_eval:.0f} changed/cyc={chg_tot:.0f}")
print("top partitions by work (runs*size):")
top = sorted(parts, key=lambda p: -runs.get(p, 0) * parts[p]['size'])
for p in top[:int(sys.argv[3]) if len(sys.argv) > 3 else 25]:
    d = parts[p]
    r = runs.get(p, 0) / C; a = anyc.get(p, 0) / C
    os_ = [o for o in outs if outs[o]['part'] == p]
    nch = sum(1 for o in os_ if chg.get(o, 0))
    names = sorted(os_, key=lambda o: -chg.get(o, 0))
    ex = ' '.join(outs[o]['name'].split('.')[-1] + ':%.2f' % (chg.get(o, 0) / C) for o in names[:4])
    print(f"P{p} k{d['kind']} size={d['size']} st={d['nst']} in={d['nin']} out={d['nout']} cmp={len(os_)} run={r:.2f} any={a:.2f} work={r*d['size']:.0f} outs_ever_chg={nch} {ex}")
# Inputs of wasted partitions: change rate of each input (by name, from output change counters)
ins = collections.defaultdict(list)
for line in open(mapf):
    f = line.split()
    if f[0] == 'I': ins[int(f[1])].append(f[2])
if ins:
    rate = collections.Counter()
    for o, d in outs.items(): rate[d['name']] += chg.get(o, 0) / C
    print("wasted partitions (run - any) * size, top inputs by change rate:")
    top = sorted(parts, key=lambda p: -(runs.get(p, 0) - anyc.get(p, 0)) * parts[p]['size'])
    for p in top[:int(sys.argv[4]) if len(sys.argv) > 4 else 15]:
        r = runs.get(p, 0) / C; a = anyc.get(p, 0) / C
        tops = sorted(ins[p], key=lambda n: -rate[n])[:5]
        print(f"P{p} size={parts[p]['size']} run={r:.2f} any={a:.2f} " + ' '.join('%s:%.2f' % (n.split('.')[-1], rate[n]) for n in tops))
