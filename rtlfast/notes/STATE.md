# State

Where things stand and what comes next. Overwrite this file at the end of every run.

- Phase: P1 done (option + subset checker), P2 in progress (activity-driven nba evaluation).
- Last verified HEAD: see LEDGER.md (latest line with a PASS verdict). As of run 2:
  0d92231, gate PASS, rocket 3.28x faster, smallboom 0.84x (slower).

## What exists
- `--rtl-fast` option (src/V3Options.*), docs in docs/guide/exe_verilator.rst, bin/verilator help.
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
    - contiguous partitions: GATED (pure, size limit 200 nodes, env
      VERILATOR_RTLFAST_PARTITION_SIZE for experiments), COND (impure `if (condTemp)`,
      flag set when a cond temp changes, re-set inside the body while it holds),
      ALWAYS (other impure statements), impure groups limited to 4000 nodes (compile time).
    - change detection at write sites (`tmp = rhs; if (lhs != tmp) {lhs = tmp; mark}`) for
      simple targets (var, array/bit selects with pure index), snapshot otherwise.
  - Debug: `--debugi-V3RtlFast 3` dumps the tree.
- rtlfast/tools/build.sh: build one benchmark with the oracle harness for profiling.

## Measurements (see LEDGER for verdict paths)
- rocket: stock ~157k Ir/cycle, v1 77k, v2 29k. perf 3.28x (56k -> 184k cycles/s).
- smallboom: v2 117k Ir/cycle, perf 0.84x (slower than stock!). Being investigated.
- Build: DUT C++ make is 3x faster than stock (many small functions), but numbers in verdicts
  are polluted by ccache hits (ref make sometimes 24 s); use CCACHE_DISABLE=1 for build studies.
- Coverage drops slightly (0.992 rocket) because V3Localize localizes comb temporaries that
  now live inside one partition function.

## Next steps
1. smallboom slowdown: compare stock and v2 profiles (callgrind) and fix.
2. Cheaper flag checks (bitmap words, skip 64 partitions at once) to allow smaller partitions.
3. Robustness: test_regress vlt scenario with `--rtl-fast` on self-checking tests.
4. P3: dedicated eval for designs whose clocks are top-level inputs.
