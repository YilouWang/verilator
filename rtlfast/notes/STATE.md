# State

Where things stand and what comes next. Overwrite this file at the end of every run.

- Phase: P1 done (option + subset checker), P2 working (activity-driven nba evaluation).
- Last perf verdicts: 1a5b10e (results/20261019-1a5b10e): gate PASS, rocket 4.57x,
  smallboom 2.41x faster than stock (both significance faster), coverage 0.988 / 0.993.
  Later commits up to HEAD only reformat code / notes (gate check verdicts in results/).
- Coverage falls slowly as partitions grow (V3Localize localizes comb temps that now live in
  one partition function); the gate needs >= 0.95.

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
      impure chain), priority = rank in a DFS post-order over RAW edges from the sinks,
      impure statements only when no pure one is ready (so they cluster).
    - contiguous partitions: GATED (pure, <= 800 nodes; once >= 50 nodes a partition is closed
      when the next statement is unrelated: no RAW from the partition and shares < half of
      its inputs), COND (impure `if (condTemp)`: flag set when a cond temp changes, re-set in
      the body while it holds), ALWAYS (other impure statements); impure groups <= 4000 nodes.
    - readers of a variable with writers*readers > 65536 marks are not gated (always run).
    - branch-free change detection: change bit per output, `tmp = rhs; ch |= lhs != tmp;
      lhs = tmp` at simple write sites (snapshot otherwise); at partition end each consumer
      byte flag is ORed with the change bits of the outputs it reads. Temporaries are
      BLOCKTEMP so V3Localize makes them locals.
    - dev knobs (not for users): VERILATOR_RTLFAST_PARTITION_SIZE, VERILATOR_RTLFAST_PROFILE=<map>
      (run/change counters; dump with HARNESS=rtlfast/tools/prof_harness.cpp in tools/build.sh).
- Tests: t_rtl_fast_unsup, t_rtl_fast_opt_bad, t_rtl_fast_gate (output vs stock golden),
  t_rtl_fast_many_marks. test_regress vlt with --rtl-fast on ~2200 tests: no wrong results.
- rtlfast/tools/build.sh: build one benchmark with the oracle harness (run from any cwd; checks
  that the local harness object is linked).

## Measurements (see LEDGER for verdict paths)
- rocket: stock ~157k Ir/cycle; branch-free gating ~42k Ir but IPC ~2.7.
  Activity (size 200): 159 partition runs/cycle (81 change something), 2623 outputs compared,
  234 change; ~800 statements are active in >60% of cycles (the core): inherent work.
- smallboom (size 800): 696 runs/cycle (305 change), 9519 outputs compared, 788 change.
  Top cost: single huge always_ff statements (ubtb: 2720 nodes, 227 outputs, runs 99%, changes
  4%; LSU) whose block temporaries tie all registers together.
- Time split (perf srcline): rocket change bits ~28%, write temps ~11%, logic ~40%;
  smallboom logic 42%, change bits 23%, write temps 15%, function entry 11%, flag checks 8%,
  marks 6%.
- Build (CCACHE_DISABLE=1, -j4): rocket stock 7.1+106.7 s vs 8.2+46.8 s; smallboom stock
  16.4+202.5 s vs 19.8+109.2 s. Verdict make times are polluted by ccache.
- The sandbox host speed changes between VM restarts (stock rocket 57k vs 44k c/s): compare
  ratios within one verdict only.
- Negative results (details in LEDGER): per-output mark branches, any-change guard, bitmap
  flags, statement-level or small partitions, no compares at all, inlining partitions into
  _eval_body__nba, -fno-stack-protector, producer affinity, other share thresholds and
  RELATED_MIN_SIZE values.

## Next steps
1. smallboom: split huge always_ff statements whose outputs depend on disjoint parts of the
   block (clone the block temporaries each piece needs), so registers whose inputs did not
   change are not recomputed; or gate sub-blocks inside a partition function.
2. Reduce compared outputs per run (9.5k/cycle on smallboom, 92% unchanged): e.g. partitioning
   that minimises the cut (outputs read by other partitions).
3. Wide operations (concat/shift via _vl_insert_WW) ~15% of smallboom instructions.
4. P3: dedicated eval for designs whose clocks are top-level inputs (eval overhead is small,
   ~1-2%, so low priority).
5. P5: larger designs (LargeBoom) through GATE_PROPOSALS.md.

## Operational notes
- Disk (~42 GB) fills up quickly: delete build dirs and test_regress/obj_vlt after use.
- Do not build in /workspace/verilator while the gate runs; develop in a separate worktree.
- Long `sleep` in one tool call (>~2 min) sometimes crashes the tool; VM restarts kill the
  heartbeat (restart it; script in the run's /workspace/heartbeat.sh pattern).
