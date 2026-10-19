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
