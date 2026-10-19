# State

Where things stand and what comes next. Overwrite this file at the end of every run.

- Phase: P1 done (option + subset checker), P2 working (activity-driven nba evaluation).
- Last perf verdicts: 1a5b10e (results/20261019-1a5b10e): gate PASS, rocket 4.57x,
  smallboom 2.41x faster than stock (both significance faster), coverage 0.988 / 0.993.
  Coverage falls slowly as partitions grow (V3Localize localizes comb temps that now live in
  one partition function); keep an eye on it (gate needs >= 0.95).

## What exists
- `--rtl-fast` option (src/V3Options.*), docs in docs/guide/exe_verilator.rst, bin/verilator help.
- src/V3DfgOptimizer.cpp: binToOneHot decoder tables are not created under --rtl-fast.
- src/V3RtlFast.cpp:
  - `checkSubset` (after linking): rejects timing controls, fork, classes, DPI, `$c`;
    options: rejects `--timing`, `--threads` > 1. Tests: t_rtl_fast_unsup, t_rtl_fast_opt_bad.
  - `gateActivity` (after V3Dead, scoped, before V3Trace), see the file header comment:
    - statements of `_eval_body__nba` analysed (reads, read-before-write, writes, purity);
      pure AstCExpr (assertion control) makes a statement impure, not unknown.
    - impure `if (c)` with pure c: c computed into `__Vrtlfast_condN` by a pure statement.
    - per trigger domain: reorder = Kahn topological sort of conflict edges (RAW, WAR, WAW,
      impure chain), priority = rank in a DFS post-order over RAW edges from the sinks,
      impure statements only when no pure one is ready (so they cluster).
    - contiguous partitions, closed early (once >= 50 nodes) when the next statement is
      unrelated (no RAW from the partition, shares < half its inputs): GATED (pure, at most 800 nodes since 1a5b10e, env
      VERILATOR_RTLFAST_PARTITION_SIZE for experiments), COND (impure `if (condTemp)`: flag set when a cond temp changes, re-set
      inside the body while it holds), ALWAYS (other impure statements); impure groups <= 4000.
    - branch-free change detection: change bit per output, `tmp = rhs; ch |= lhs != tmp;
      lhs = tmp` at simple write sites (snapshot otherwise), consumer byte flags ORed with the
      change bits at partition end. Temporaries are BLOCKTEMP so they become locals.
    - readers of a variable with writers*readers > 65536 marks are not gated (always run);
      this bounds verilation memory (t_flag_output_groups_max_size N=20000 used >9 GB).
    - dev profiling: VERILATOR_RTLFAST_PROFILE=<map> adds run/change counters;
      HARNESS=rtlfast/tools/prof_harness.cpp in tools/build.sh dumps them (prof-counters.txt).
- rtlfast/tools/build.sh: build one benchmark with the oracle harness (checks the link line).
- Tests: t_rtl_fast_unsup, t_rtl_fast_opt_bad, t_rtl_fast_gate (output vs stock golden),
  t_rtl_fast_many_marks. test_regress vlt with --rtl-fast on 1181 tests: no wrong results.

## Measurements (see LEDGER for verdict paths)
- rocket: stock ~157k Ir/cycle; v2 (branchy) 29k; branch-free 42k Ir but same time.
  Activity: 159 partition runs/cycle (81 change something), 2623 outputs compared, 234 change.
  ~800 statements are active in >60% of cycles (the core): inherent work.
- smallboom: 562 runs/cycle, 11.6k outputs compared, 807 change. Large LSU/issue-queue blocks
  with many outputs (block temps of always_ff, `unnamedblk2._GEN_*`) dominate.
- Time split (rocket, perf srcline, rough): change bits ~28%, write temps ~11%, flag marks 5%,
  flag checks 5%, function entry 5%, logic ~40%.
- Negative results: per-output mark branches (mispredicts), bitmap flags, smaller partitions
  (per-partition overhead), no compares at all, BLOCKTEMP had no measurable effect,
  producer affinity, other input-share thresholds than 50%.
- smallboom time: flag checks in _eval_body__nba ~10% (2750 data dependent branches);
  two-level group flags would save only ~15% of checks if activity were independent.
- Build (CCACHE_DISABLE=1, -j4): rocket stock 7.1+106.7 s vs 8.2+46.8 s; smallboom stock
  16.4+202.5 s vs 19.8+109.2 s. Verdict make times are polluted by ccache.
- The sandbox host speed changes between VM restarts (stock rocket 57k vs 44k c/s).

## Next steps
1. Smallboom: why only 1.8x. Big always_ff blocks with block temps; consider splitting
   statements whose outputs are independent, or reducing outputs compared per run.
2. Wide operations (concat/shift via _vl_insert_WW) are ~15% of smallboom instructions.
3. Robustness: more test_regress families with --rtl-fast; tests for the new code paths.
4. P3: dedicated eval for designs whose clocks are top-level inputs.
