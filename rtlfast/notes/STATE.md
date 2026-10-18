# State

Where things stand and what comes next. Overwrite this file at the end of every run.

- Phase: P1 done (option + subset checker), P2 in progress (activity-driven nba evaluation).
- Last verified HEAD: see LEDGER.md (latest line with a PASS verdict).

## What exists
- `--rtl-fast` option (src/V3Options.*), docs in docs/guide/exe_verilator.rst, bin/verilator help.
- src/V3RtlFast.cpp:
  - `checkSubset` (after linking): rejects timing controls, fork, classes, DPI, `$c`;
    options: rejects `--timing`, `--threads` > 1. Tests: t_rtl_fast_unsup, t_rtl_fast_opt_bad.
  - `gateActivity` (after V3Dead, scoped, before V3Trace): activity gating of the nba
    region. Statements of `_eval_body__nba` are analysed (read-before-write sets, writes,
    purity), reordered inside each trigger domain (any order that keeps all conflicting
    statement pairs and impure statements in order), cut into partitions with a dirty flag
    each. Outputs are compared (at the write site for single whole assignments, else
    snapshot before/after) and readers are marked dirty on change. Variables written
    outside the nba region at run time are compared against a shadow at region start.
    Impure `if (c) ...` statements get `c` precomputed in a gated statement; groups of such
    statements are gated on their conditions and stay dirty while a condition holds.
  - Debug: `--debugi-V3RtlFast 3` writes `<prefix>_NNN_rtlfast_parts.txt` (partition
    inputs/outputs). Env `VERILATOR_RTLFAST_PARTITION_SIZE` (default 200 nodes) for tuning.

## Measurements (rocket, see LEDGER for verdict paths)
- Stock rocket: verilate 7 s, C++ make 155-217 s (-j4). DUT with --rtl-fast: make ~32 s.
- Stock: ~157k instructions/cycle (callgrind). Gating v1: 77k; with reorder and
  write-site compare: ~67k.
- The rocket core is active about 90% of cycles; the rest of the SoC is mostly idle.

## Next steps
1. Tune partition size, clustering of register commits (`x = __Vdly__x`) and flag checks.
2. Prevent extra localization of variables (coverage safety): variables that stock keeps
   as members should stay members.
3. Robustness: run test_regress vlt scenario with `--rtl-fast` passed down
   (`test_regress/driver.py --vlt --rtl-fast t/t_xxx.py` passes unknown dash options to
   Verilator) on a sample of self-checking tests.
4. P3: dedicated eval for designs whose clocks are top-level inputs.
