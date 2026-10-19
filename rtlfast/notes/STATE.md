# State

Where things stand and what comes next. Overwrite this file at the end of every run.

- Phase: P1 done (option + subset checker), P2 working (activity-driven nba evaluation).
- Last perf verdicts: b880403 (results/20261019-b880403): gate PASS, rocket 4.46x, smallboom 3.35x
  (best), coverage 1.0 / 1.0. Re-run at the end of run 4 on the same source (results/20261019-6364dfb):
  rocket 4.35x (56.7k -> 246.7k), smallboom 2.62x (21.2k -> 55.4k). d93d1d3 (run 3): 5.41x / 2.97x.
- HOST MATTERS: run 4 ran on a Xeon 2.1 GHz with 2 MiB L2 per core and 260 MiB shared L3 (run 3: 2.8 GHz,
  1 MiB L2, 33 MiB L3). Stock's large straight-line code profits from big, quiet caches much more than the
  gated model: stock smallboom moved 15.1k -> 21.2k c/s within run 4 while the dut moved 50.5k -> 55.4k.
  Ratios are only comparable within one verdict or one interleaved A/B; record `lscpu` with every verdict.
- HEAD src = 0651d80 (gate check PASS, results/20261019-0651d80) = b880403 plus a statistics counter
  and a test; no code generation change since b880403.
- Coverage: under --rtl-fast V3Localize keeps user signals (not internal) as members, so
  coverage no longer depends on partition shapes (47e82bb).

## What exists
- `--rtl-fast` option (src/V3Options.*), docs in docs/guide/exe_verilator.rst, bin/verilator help.
- src/V3DfgOptimizer.cpp: binToOneHot decoder tables are not created under --rtl-fast
  (tab[pre]=0; tab[idx]=1 rewrites the table every evaluation and keeps all readers active).
- src/V3RtlFast.cpp (see its header comment):
  - `checkSubset` (after linking): rejects timing controls, fork, classes, DPI, `$c`;
    options: rejects `--timing`, `--threads` > 1.
  - `gateActivity` (after V3Dead, scoped, before V3Trace):
    - statements of `_eval_body__nba` analysed (reads, read-before-write, writes, purity);
      a pure AstCExpr (assertion control) makes a statement impure, not unknown.
    - impure `if (c)` with pure c: c computed into `__Vrtlfast_condN` by a pure statement.
    - per trigger domain: reorder = Kahn topological sort of conflict edges (RAW, WAR, WAW,
      impure chain); DFS roots (sinks) sorted by the name of their first written variable;
      impure chain), priority = rank in a DFS post-order over RAW edges from the sinks,
      impure statements only when no pure one is ready (so they cluster).
    - contiguous partitions: GATED (pure, <= 800 nodes; once >= 50 nodes a partition is closed
      when the next statement is unrelated: no RAW from the partition and shares < half of
      its inputs), COND (impure `if (condTemp)`: flag set when a cond temp changes, re-set in
      the body while it holds), ALWAYS (other impure statements); impure groups <= 4000 nodes.
    - readers of a variable with writers*readers > 65536 marks are not gated (always run).
    - change detection covers every packed type (basic types except opaque ones, packed arrays,
      packed structs and unions; b880403); outputs of other types are assumed changed whenever
      their writer runs (stat "RtlFast, outputs not compared").
    - branch-free change detection: change bit per output, `tmp = rhs; ch |= lhs != tmp;
      lhs = tmp` at simple write sites (snapshot otherwise); at partition end each consumer
      byte flag is ORed with the change bits of the outputs it reads. Temporaries are
      BLOCKTEMP so V3Localize makes them locals.
    - bundles: consecutive gated partitions of <= 150 nodes share one function (<= 800 nodes),
      called when one member flag is set; each member checks its own flag.
    - block temporaries that cross partitions or are partition inputs become MODULETEMP, so
      V3Localize does not turn them into function locals (needed for bundles).
    - change bits are shared by outputs with identical consumer lists; a partition does not mark
      itself for variables that are its inputs only because of other writers.
    - partition functions carry AstCFunc::optSize -> VL_ATTR_OPTSIZE (GCC optimize("Os"), clang
      minsize): the gated model is instruction-fetch bound (smallboom +12-15%; stock gets slower
      at -Os, so this is specific to the gated code).
    - dev knobs (not for users): VERILATOR_RTLFAST_BUNDLE_MEMBER / _BUNDLE, VERILATOR_RTLFAST_PARTITION_SIZE, VERILATOR_RTLFAST_PROFILE=<map>
      (run/change counters; dump with HARNESS=rtlfast/tools/prof_harness.cpp in tools/build.sh).
- Tests: t_rtl_fast_unsup, t_rtl_fast_opt_bad, t_rtl_fast_gate, t_rtl_fast_bundle and
  t_rtl_fast_packed (output vs stock golden; packed uses VERILATOR_RTLFAST_PARTITION_SIZE=1 and
  checks "outputs not compared 0"), t_rtl_fast_many_marks. test_regress vlt with --rtl-fast at d93d1d3 on ~3100 tests
  (lists: run-3 LEDGER): no wrong results; VCD-golden failures also fail on stock, VCDs identical.
- rtlfast/tools/prof_analyze.py <map> <prof-counters.txt> [n] [m]: partition activity table and the
  inputs that trigger runs without output change (needs a VERILATOR_RTLFAST_PROFILE build).
- rtlfast/tools/build.sh: build one benchmark with the oracle harness (run from any cwd; checks
  that the local harness object is linked).

## Measurements (see LEDGER for verdict paths)
- Cost model (run 4): time follows the bytes of code executed per cycle (front-end bound, no HW
  counters in the sandbox). smallboom (b880403) executes ~680 KB of code per cycle (2.85 MB of
  partition code in total; _eval_body__nba 63 KB, which includes partition functions GCC inlined).
  Hot instructions: logic ~70%, change detection (cmp/setne/or) ~20%, dirty-flag operations ~9%.
- Per-function perf joined with profile counters (smallboom): partition functions 83% of time,
  about half of it (42%) in runs that change no output; bundle members (small partitions) take
  44% of the time for 34% of the work (nodes). Rocket: 79% / 38.5%.
- "Work" in nodes overstates runs whose body is mostly skipped by an internal if/?: : such runs
  are cheap. Removing them (guard-aware marks) cut runs by 12% but did not save time.
- Activity: smallboom (b880403) 712.7 partition runs/cycle (312 change), work 121.3k of 405k nodes;
  rocket 183 runs/cycle (67 change), work 38.0k of 205k nodes (18.5%), 1816 outputs compared, 218
  change. All runs are explained by input changes (no spurious marks left after b880403).
- Remaining "wasted" runs come from data inputs that change every cycle and are masked inside the
  partition: frontend PCs (s1_pc, f3_pc, f4_idx), pipeline registers (r_uop_*, ex_reg_inst), free
  running counters (mcycle/instret/time feeding the CSR read mux).
- Build (CCACHE_DISABLE=1, -j4): rocket stock 7.1+106.7 s vs 8.2+46.8 s; smallboom stock
  16.4+202.5 s vs 19.8+109.2 s. Verdict make times are polluted by ccache.
- Host noise: single runs spread +-15% this run even pinned to a core; effects below ~5% cannot be
  resolved with 5-10 rounds. Host speed also changes between VM restarts.
- Negative results (details in LEDGER): per-output mark branches, any-change guard, bitmap
  flags, statement-level or small partitions, no compares at all, inlining partitions into
  _eval_body__nba, -fno-stack-protector, producer affinity, other share thresholds and
  RELATED_MIN_SIZE values; run 4: branch around high-fanout marks, guard-aware marks, variable
  layout in partition order, 64-bit bundle flag checks, align-functions under optimize("Os"),
  select-from-concatenation rewrite (journal wip/selconcat-neutral.patch).

## LargeBoom (proposed for the gate, see GATE_PROPOSALS.md)
- b880403/0651d80 (run 4): lockstep 50k PASS (cov 1.0); perf 1.226x (3.97k -> 4.86k c/s) on a host that
  is much slower for LargeBoom than in run 3 (a 30k-cycle check of the same binaries gave ~1.45x).
- d93d1d3: lockstep 50k PASS (cov 1.0); perf 1.350x (6.3k -> 8.5k c/s), +23% from VL_ATTR_OPTSIZE.
- fb45a89: lockstep 50k PASS (cov 1.0); perf 1.163x only (5.9k -> 6.9k c/s); stock build 29 min,
  rtl-fast C++ build 6x faster. Scratch setup: design.json from GATE_PROPOSALS.md, sources from
  difftest/TestHarness-LargeBoom.tar.bz2 of the pinned gsim-ready-to-run commit.
- 4005 partitions, 1463 runs/cycle, 333k nodes/cycle (188k without output change); per-node cost
  1.7x that of smallboom (cache pressure). Fission of >= 2000-node statements: no gain.

## Next steps
1. LargeBoom: re-profile with the packed fix (perf per function joined with profile counters, as in
   run 4) and look for pathologies like the packed one: ask "why does this partition run?" for the
   most expensive functions. That is the method that found the only win of run 4.
2. Change detection is ~20% of hot instructions. The remaining structural idea: lay out the outputs
   of each change group contiguously (V3VariableOrder keeps the AstVar order within a size class,
   so V3RtlFast can reorder AstVars; run 4 checked that reordering alone is neutral) and compare a
   group with one snapshot + memcmp instead of per-output compares. Needs a node or emitter support
   for "address range of these members" and a check at emit time that the members are contiguous.
   Gain only where groups have several outputs (rocket: 10604 outputs share 4690 change bits).
3. Do not branch on change bits in hot code (three negative results now: per-output mark branches,
   one branch around all marks, guards evaluated under 'if (ch)' = 0.65x).
4. Masked high-rate inputs (see Measurements) are the main source of wasted runs. Guard-aware marks
   did not pay; what might: splitting straight-line mux statements (CSR read data, TLB entry
   matches) so that the masked terms become separate statements whose outputs do not change.
   Needs a cost model in bytes executed, not nodes.
5. Code sharing between repeated instances (P4, ARMOR-style packing of isomorphic narrow nodes)
   would cut code bytes, but needs un-inlined modules and per-instance flags; large project.
6. P3 (dedicated eval for top-level clocks) remains low priority (eval overhead ~1-2%).

## Operational notes
- Disk (~42 GB) fills up quickly: delete build dirs and test_regress/obj_vlt after use.
- Do not build in /workspace/verilator while the gate runs; develop in a separate worktree.
- Long `sleep` in one tool call (>~2 min) sometimes crashes the tool; VM restarts kill the
  heartbeat (restart it; script in the run's /workspace/heartbeat.sh pattern).
- Run 4 helpers (recreate if lost): /workspace/rungate.sh <sha> [check|perf|both] (ff-merge dev commit
  into /workspace/verilator, rebuild, gate); /workspace/p/ab.sh <cycles> <rounds> dirs... (interleaved
  runs pinned to core 2, medians); func2part map from a profile build (grep prof_runs[N] per function)
  to join perf per-function samples with profile counters.
- Run 3 workflow that worked: develop in a second worktree (/workspace/dev), commit there, then
  fast-forward /workspace/verilator to the commit, rebuild and run gate check + perf in the
  background; A/B builds with rtlfast/tools/build.sh and interleaved runs; always diff the Linux
  boot text of A and B before trusting a speedup. Keep tool-call sleeps under ~90 s.
