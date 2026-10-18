# Ledger

Append one line per experiment. Never edit old lines.
Format: `UTC date | short sha | what was tried | check verdict | perf (speedup / build time) | conclusion`

2026-10-18 | 54730b2 | oracle self-test A: stock vs stock --threads 2, rocket 200k cycles | PASS, coverage 0.976 | n/a | an equivalent but differently scheduled build passes
2026-10-18 | 54730b2 | oracle self-test B: rocket ALU carry-in mutated only when in1[40]&in2[40]&in1[3] | FAIL at cycle 188401, 3 members (alu io_adder_out, alu out, dcacheArb req addr); design text still identical | n/a | the per-cycle state digest catches a bug the console output does not show, and localises it
2026-10-18 | 54730b2 | perf sanity: stock vs stock, rocket 200k cycles x3 | n/a | 0.94x (ref 58k to 63k, dut 50k to 70k cycles/s) | sandbox timing noise is about 15 to 30 percent; perf now reports significance
2026-10-18 | 54730b2 | activity on rocket Linux boot, 300k cycles | n/a | 2.64% of 5921 state members change per cycle | most per-cycle work is redundant
