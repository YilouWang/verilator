# Gate proposals

Designs proposed for the gate, with their design.json and why. Yilou adds accepted ones to the oracle store.

## largeboom (proposed 2026-10-19, run 3)

Why: the next size up from smallboom (49.6k nba statements vs 30k; stock C++ build 29 min), the
first design where --rtl-fast scales badly (1.16x vs 2.57x on smallboom), so it should guard and
drive the work on large designs. Same source repository and Linux image as the current gate:
`difftest/TestHarness-LargeBoom.tar.bz2` in jaypiper/gsim-ready-to-run at the pinned commit
55803f6a (bench/fetch.sh would need one more `get largeboom TestHarness-LargeBoom.tar.bz2
TestHarness-LargeBoom.sv` line).

design.json (identical to smallboom apart from name and source):

```json
{
  "name": "largeboom",
  "top": "TestHarness",
  "sources": ["src/TestHarness-LargeBoom.sv"],
  "verilator_flags": ["--max-num-width", "1048576", "+define+RANDOMIZE_GARBAGE_ASSIGN"],
  "clock": "clock",
  "reset": "reset",
  "reset_active": 1,
  "reset_cycles": 10,
  "preload": [
    {"member": "TestHarness__DOT__mem__DOT__srams__DOT__mem_ext__DOT__Memory", "file": "src/linux.bin"}
  ],
  "workload": "first N cycles of the GSIM ready-to-run Linux boot image"
}
```

Evidence at fb45a89 (results/20261019-fb45a89/extra-*-largeboom*.json): lockstep check 50k cycles
PASS, coverage 1.0; perf 100k cycles x3: stock 5.9k, --rtl-fast 6.9k c/s (1.163x, faster).
Build: stock verilate 51 s + make 1762 s, --rtl-fast 57 s + 289 s (-j3, no ccache for either).
Update at d93d1d3 (results/20261019-d93d1d3/extra-*-largeboom*.json): check 50k PASS, coverage 1.0;
perf 1.350x (6.3k -> 8.5k c/s).
Suggested gate entry: check 50000 cycles seed 1, perf 100000 cycles repeat 3 (a stock build alone
takes ~30 min of the run budget).

## Random-input designs: picorv32, ibex, ibex-full, veer-el2 (proposed 2026-10-19, run 5)

Why: the gate has only two Chisel/CIRCT-generated designs with one workload (Linux boot). These
hand-written designs cover SystemVerilog packages, packed structs, enums, interfaces (VeeR
el2_mem_if), generate, latch-based clock gating (ibex prim_clock_gating, VeeR rvclkhdr) and a
very different activity regime (all inputs random every cycle). They are small: a check of
200k cycles takes 1 to 3 minutes per design including both builds.

Setup: `rtlfast/extra/setup.sh <dir>` clones the pinned commits (picorv32 ef203c2b, ibex
1b29dae8, Cores-VeeR-EL2 0169d669), runs the VeeR config script and writes
`<dir>/<name>/design.json` (absolute paths; ibex needs `+define+SYNTHESIS`, otherwise its
simulation-only DPI exports are rejected by --rtl-fast).

Evidence at 0651d80 sources (results/20261019-3fb91a5/extra-*.json): check PASS with coverage
1.0 for picorv32 seeds 1-3, ibex seeds 1-2, ibex-full seeds 1-3, veer-el2 seeds 1-2 (200k
cycles, veer-el2 100k). Perf: picorv32 1.20x, veer-el2 1.39x faster, ibex-full 0.70x SLOWER
(about 75% of the nba nodes are active every cycle with random inputs, and the nba region runs
~1.5 times per cycle). A perf entry for ibex would guard against making high-activity designs
even slower.

Update at 694f938 (results/20261019-7b18a74): nba regions below 50000 AST nodes are no longer
gated, so picorv32, ibex, ibex-full and VexRiscv (also added, SpinalHDL, needs --flatten) run at
~1.0x (within noise) and only check the subset checker and the untouched code paths; veer-el2
(104k nodes) is still gated: check PASS, perf 1.43x.

Suggested gate entries: check 200000 cycles seeds [1, 2] for each (veer-el2 is the one that
exercises gating; the small ones guard the ungated path); perf veer-el2 500000 cycles, repeat 3.
