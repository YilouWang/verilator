# Ledger

Append one line per experiment. Never edit old lines.
Format: `UTC date | short sha | what was tried | check verdict | perf (speedup / build time) | conclusion`

2026-10-18 | 54730b2 | oracle self-test A: stock vs stock --threads 2, rocket 200k cycles | PASS, coverage 0.976 | n/a | an equivalent but differently scheduled build passes
2026-10-18 | 54730b2 | oracle self-test B: rocket ALU carry-in mutated only when in1[40]&in2[40]&in1[3] | FAIL at cycle 188401, 3 members (alu io_adder_out, alu out, dcacheArb req addr); design text still identical | n/a | the per-cycle state digest catches a bug the console output does not show, and localises it
2026-10-18 | 54730b2 | perf sanity: stock vs stock, rocket 200k cycles x3 | n/a | 0.94x (ref 58k to 63k, dut 50k to 70k cycles/s) | sandbox timing noise is about 15 to 30 percent; perf now reports significance
2026-10-18 | 54730b2 | activity on rocket Linux boot, 300k cycles | n/a | 2.64% of 5921 state members change per cycle | most per-cycle work is redundant
2026-10-18 | 5ec43ef | bootstrap gate on the kit commit (stock vs stock) | PASS rocket+smallboom, coverage 1.0 | rocket make 155 s, smallboom make 323 s (stock, -j4) | baseline; C++ compile dominates build time (verilate 7 s / 16 s)
2026-10-18 | 403a5ef | --rtl-fast gating v1: contiguous partitions (200 nodes) of the nba statements in V3Order order, dirty flags, snapshot compare | PASS rocket 300k + smallboom 150k, coverage 1.0 (results/20261018-403a5ef) | rocket 1.191x faster (60.8k -> 72.4k c/s); rocket make 217 s -> 32 s; smallboom 0.976x within_noise, make unchanged | works on rocket; smallboom apparently not gated (to investigate)
