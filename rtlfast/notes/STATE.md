# State

Where things stand and what comes next. Overwrite this file at the end of every run.

- Phase: P1 done (option + subset checker), P2 working (activity-driven nba evaluation), P5 started
  (extra designs with random inputs, rtlfast/extra/setup.sh).
- HEAD src = 1dd1376 (run 5): 0651d80 plus DPI exports allowed (06d3124), no gating for nba
  regions below 50000 AST nodes (694f938), change detection at outside assignment sites instead
  of shadow copies (1dd1376: ibex-full gated 1.15x, VeeR 1.17x). Gate check PASS
  (results/20261019-1dd1376). Code
  generation for the gate designs unchanged since b880403; current perf on the run-5 host
  (results/20261019-6200f62): rocket 5.008x, smallboom 3.103x.
- Last perf verdicts: b880403 (results/20261019-b880403): rocket 4.46x, smallboom 3.35x; end of run 4
  re-run (results/20261019-6364dfb): rocket 4.35x, smallboom 2.62x; d93d1d3 (run 3): 5.41x / 2.97x.
  Run 5 did not produce a faster revision (all experiments neutral or negative, see LEDGER).
- HOST MATTERS: run 4 ran on a Xeon 2.1 GHz with 2 MiB L2 per core and 260 MiB L3, runs 3 and 5 on a
  Xeon 2.8 GHz with 1 MiB L2 and 33 MiB L3. Stock profits from big caches much more than the gated
  model. Ratios are only comparable within one verdict or one interleaved A/B; record `lscpu`.
- Coverage: under --rtl-fast V3Localize keeps user signals (not internal) as members, so
  coverage no longer depends on partition shapes (47e82bb).
- Known weakness (run 5, mitigated by 694f938 for small designs): designs with very high activity get SLOWER: ibex with random inputs
  0.64x to 0.70x (75% of nba nodes active, nba region ~1.5 times per cycle, 528 variables written by
  replicated ico/act logic compared with shadows at every nba region start = 32% of the time).
  picorv32 1.2x and VeeR EL2 1.39x faster with random inputs. Since 694f938 regions below 50k nodes
  are not gated (ibex, ibex-full, VexRiscv, picorv32 ~1.0x; VeeR 104k nodes still gated, 1.43x).

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
    - nba regions below MIN_NODES = 50000 AST nodes are not gated (694f938; env
      VERILATOR_RTLFAST_MIN_NODES, gating tests set it to 0).
    - variables written outside the nba region by plain assignments (ico/act logic, still
      AstAssignW at this stage) compare and mark their consumers at the assignment (1dd1376);
      shadow copies only for top inputs, public/DPI-written variables and method calls.
    - DPI exports are allowed (06d3124): variables they write are outside writes (shadow compare);
      DPI imports, timing, fork, classes and $c are rejected.
    - dev knobs (not for users): VERILATOR_RTLFAST_BUNDLE_MEMBER / _BUNDLE, VERILATOR_RTLFAST_PARTITION_SIZE, VERILATOR_RTLFAST_PROFILE=<map>
      (run/change counters; dump with HARNESS=rtlfast/tools/prof_harness.cpp in tools/build.sh).
- Tests: t_rtl_fast_unsup, t_rtl_fast_opt_bad, t_rtl_fast_gate, t_rtl_fast_bundle, t_rtl_fast_dpi_export,
  t_rtl_fast_small, t_rtl_fast_outside and
  t_rtl_fast_packed (output vs stock golden; packed uses VERILATOR_RTLFAST_PARTITION_SIZE=1 and
  checks "outputs not compared 0"), t_rtl_fast_many_marks. test_regress vlt with --rtl-fast at d93d1d3 on ~3100 tests
  (lists: run-3 LEDGER): no wrong results; VCD-golden failures also fail on stock, VCDs identical.
- rtlfast/tools/prof_analyze.py <map> <prof-counters.txt> [n] [m]: partition activity table and the
  inputs that trigger runs without output change (needs a VERILATOR_RTLFAST_PROFILE build).
- rtlfast/extra/setup.sh <dir>: picorv32, ibex (default and full config), VeeR EL2 at pinned commits with
  design.json files for lockstep.py (random inputs). Verdicts in results/20261019-3fb91a5/extra-*.json.
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
1. Run 5 closed most cheap performance directions for the gated code (all in LEDGER with numbers):
   base pointers (code -7 to -11%, time +0 to +3%; journal wip/basepointers-neutral.patch), software
   prefetch of partition code (worse), affinity rule for new inputs, high-fanout isolation, guards
   on LargeBoom (all neutral or worse). Without hardware counters the bottleneck of the gated model
   cannot be pinned down; the evidence says per-run work (compares ~20%, marks ~14% of executed
   instructions on LargeBoom), not code size or fetch latency.
2. Designs driven by a testbench: 1dd1376 moved change detection of ico/act logic to the write
   sites (+15-17%). Next for them: profile VeeR/ibex again (top-input shadows, the ico region itself
   runs every evaluation in stock and in rtl-fast alike).
3. Structural options left for large designs: (a) sharing code between repeated instances (needs
   per-instance layout, parametrised constants, aligned partitioning; LargeBoom exact-duplicate
   potential only 12% of executed text), (b) a second, ungated code copy chosen at run time for very
   active phases (doubles nba compile time).
4. MIN_NODES (50000) is a size proxy for "activity too high to gain"; a large design with very high
   activity can still be slower. Revisit if a gate or extra design shows that.
5. test_regress sweeps with --rtl-fast must set VERILATOR_RTLFAST_MIN_NODES=0, otherwise the small
   test designs are not gated.
6. Ask Yilou about the gate proposals (LargeBoom; extra random-input designs, VeeR EL2 gated).
7. Do not branch on change bits in hot code (three negative results).

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
