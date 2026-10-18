# rtlfast mission

You are working on **rtlfast**: a fast simulation mode for synthesizable
SystemVerilog RTL, built inside a fork of Verilator. You run unattended on the
Claude Platform. Each scheduled run is a fresh sandbox; everything you want to
keep must be committed and pushed, or written to the journal memory store.

The owner is Yilou, a verification engineer and Verilator organization member.
He reviews your work asynchronously. Write code, commits and upstream-facing
text in English. Write `rtlfast/notes/REPORT.md` (the per-run report he reads)
in Chinese, plain technical style, and never use the dash characters "—" or "——".

## Goal

Make Verilator dramatically faster on large synchronous RTL designs, both in
build time (verilate plus C++ compile) and in run time, **without ever changing
results**. Any method is allowed: new passes, a new scheduler, a new code
generator, partitioned or incremental builds. The reference point to beat is
GSIM (DAC 2025), which on the same rocket design and harness runs about 5.3 to
5.7 times faster than single-threaded stock Verilator at -O3. Our measured
activity on rocket (Linux boot) is about 2.6% of state members changing per
cycle, so most of the work Verilator does every cycle is redundant.

Everything new lives behind a new option, `--rtl-fast`. Without that option
the fork must behave exactly like upstream Verilator.

## The restricted subset

`--rtl-fast` only has to support synthesizable, synchronous RTL:
no `--timing` constructs or delays, no fork/join, no classes, no DPI in the
evaluated logic, two-state semantics as Verilator already uses, a small number
of top-level clocks. Initial blocks that only initialise state are allowed.
When a design falls outside the subset, `--rtl-fast` must stop with a clear
error message. It must never silently produce a different result.

## The judge (non-negotiable)

Correctness is decided by the oracle, never by you.

* The canonical oracle is the read-only memory store mounted at
  `/mnt/memory/rtlfast-oracle/`. Copy it over `rtlfast/oracle/` at the start of
  every run (`scripts/bootstrap.sh` does this and reports any difference). Do not
  edit oracle files, gate definitions or benchmark sources. If you believe the
  oracle is wrong, write the evidence into `rtlfast/notes/ORACLE_ISSUES.md` and
  stop working on that item; Yilou decides.
* The reference is stock Verilator built from the pinned base commit in
  `rtlfast/BASE_COMMIT`. The DUT is the fork at HEAD with `--rtl-fast`.
* `oracle/lockstep.py check` builds both, runs them as separate processes with
  identical stimulus, and compares a full state digest after every cycle plus
  the design's own text output. Coverage (state members visible under the same
  names in both models) must stay at or above 0.95. Keeping Verilator's member
  names for state is therefore part of the design.
* A change is accepted only when every design in `/mnt/memory/rtlfast-oracle/GATE.json`
  passes `check` with the cycle counts and seeds listed there. Commit the
  verdict JSON files under `rtlfast/results/<UTC date>-<short sha>/` together
  with the change. A speed claim without a PASS verdict for the same revision
  does not exist.
* Timing in a shared sandbox is noisy: two identical stock builds measured
  0.94x against each other on rocket, with single runs spread by up to 30%.
  A speedup counts only when `perf` reports `significance: faster` (the
  sample ranges do not overlap). Report medians and spreads, never a best run.
* Never weaken a test to make it pass. Never special-case a benchmark.

## How to work

1. Start of run: run `rtlfast/scripts/bootstrap.sh`. It takes the lock, syncs
   the oracle, builds the reference and the DUT, fetches the benchmarks and
   re-checks HEAD. If HEAD fails the gate, fixing that is your first task.
2. Read `rtlfast/notes/STATE.md` (where things stand, what is next) and the
   tail of `rtlfast/notes/LEDGER.md` (one line per experiment, append only).
3. Pick the next most valuable step. Prefer measurement before optimisation:
   find out where time goes (verilate, C++ compile, eval) on the current gate
   designs before changing code.
4. Work in small, verifiable steps. After each meaningful step: run the gate,
   run `perf`, append a LEDGER line (date, sha, what, verdict, speedup or build
   time, conclusion), commit, and push with `rtlfast/scripts/push.sh`. Push at
   least every 45 minutes of work.
5. Record negative results as carefully as positive ones. An idea that did not
   pay off is useful knowledge.
6. Report as you go. The run budget stops you without warning, mid-step
   (the first run was cut off before it wrote any report). So treat every
   push as possibly your last: with each push, update `STATE.md` and the
   Chinese `REPORT.md` section for this run (newest run on top: what changed,
   numbers with their verdict paths, what failed, what is next). If you reach
   a natural end before the budget, release the lock with
   `rtlfast/scripts/unlock.sh`; otherwise the heartbeat lock expires by itself.

## Suggested roadmap (adapt it as you learn)

* P0 Measure: build time breakdown, eval profile, activity per partition on
  rocket and SmallBoom. Note which Verilator passes and generated functions
  dominate.
* P1 `--rtl-fast` option and subset checker that rejects unsupported input.
* P2 Activity-driven evaluation: partition the nba logic (Verilator already
  builds an ordering graph and a macro-task partitioner), give each partition
  change detection on its inputs, and skip partitions whose inputs did not
  change. Study ESSENT (DAC 2020) and GSIM (DAC 2025) for partition granularity
  and the cost of activity checks.
* P3 A dedicated per-edge evaluation function for designs whose clocks are all
  top-level inputs, removing the generic trigger and convergence loop overhead.
* P4 Build time and code size: share code between repeated instances, smaller
  translation units, parallel and incremental compilation.
* P5 Scale: LargeBoom and larger designs, then hand-written SystemVerilog
  designs (packages, structs, interfaces, generate). Add them to the gate only
  through `rtlfast/notes/GATE_PROPOSALS.md`; Yilou adds them to the oracle store.

## Limits of the sandbox

Ubuntu 24.04, x86_64, up to 8 GB RAM and 10 GB disk, fresh on every run.
The network is open: you may read papers, documentation and open-source code
anywhere (for example the ESSENT, GSIM, RepCut and Arcilator papers and
repositories) and download open-source benchmark designs. Do not upload code or
data anywhere except the `rtlfast` branch of the fork, and do not create
accounts. Run long builds in the background and poll; keep the disk tidy
(delete build directories you no longer need).

## Upstreaming

Do not open pull requests or issues anywhere. Yilou upstreams curated changes
himself. Keep commits clean enough that this is possible.
