# Verilator on the road to UVM: a thematic PR archaeology

Empirical basis: every PR/commit cited below is taken from `git log` on
`origin/master` (verilator/verilator upstream) inside this checkout. The
canonical sources are the per-stage CSVs produced by `harvest.sh` (see
`out/`); this report selects the most representative landmarks from each
stage, not the exhaustive list.

Notes on PR numbering. Pre-2020 entries often carry `bugNNNN` issue refs
rather than PR numbers; from late 2022 onward virtually every functional
commit is squashed with a trailing `(#NNNN)` PR ref, which is what the
harvester extracts. A small number of cited commits land without a PR ref
because the squash policy at the time omitted it; their SHAs are listed.

---

## Stage 1 — Scheduling & Time Semantics

Verilator's pre-2022 architecture was a clock-edge ranked, purely
combinational/sequential evaluator that knew nothing about the
LRM-1800 reactive/observed/re-NBA region split, and treated `#delay`
as a lint warning. Two upstream PRs in 2022 changed that.

- **PR #3384 — *IEEE compliant scheduler* (2022-05-15, `599d23697`).**
  Introduces `src/V3Sched.cpp` and the partition/replicate/acyclic
  passes underneath it. This is the structural prerequisite for
  everything downstream: it replaces the legacy `V3Order`-only flow
  with an LRM-faithful active/inactive/NBA/observed scheduling lattice
  that any timing-aware feature can plug into.

- **PR #3363 — *Timing support* (2022-08-22, `39af5d020`).** Lands
  `src/V3Timing.cpp` and the C++20-coroutine runtime in
  `verilated_timing.cpp`. This is the single PR that turns Verilator
  from a zero-delay RTL simulator into an event-driven simulator with
  `#`, `@`, `wait`, and `fork…join`. Industrially, this is *the*
  enabling commit for UVM, because UVM phase scheduling, sequencer
  delay handling, and `uvm_event` all assume coroutine-style control
  flow.

- **PR #6730 — *Fix fork scheduling semantics* (2025-11-26,
  `2c5ff3f63`)** and its repair **PR #6891** (2026-02-17). Industrial
  UVM testbenches (cv32e40p reference flow included) bury hundreds
  of `fork…join_none` constructs inside `run_phase`; this PR rewrites
  Verilator's fork process model to match LRM start/suspend/resume
  ordering and is what made multi-agent UVM environments
  *deterministic* across runs.

- **PR #7079 — *Support `#0` delays with IEEE-1800 compliant
  semantics* (2026-02-16, `505d33b35`).** UVM's `phase_done`
  callbacks and several `uvm_objection` paths rely on `#0` to defer
  scheduling within the same simulation step; before this PR
  Verilator silently coalesced or dropped them.

Supporting work between landmarks: PR #4673 *--timing triggers for
virtual interfaces* (2023-12-04) introduces `V3SchedVirtIface.cpp`
(the UVM virtual-interface trigger fast-path); PR #6620 *single
trigger vector for all scheduling regions* (2025-11-01); PR #6280's
chain of "Internals: scheduling refactor" commits across late 2025
preparing the AST for the SVA NFA engine of Stage 5.

## Stage 2 — SystemVerilog OOP (Classes & Dynamic Objects)

Verilator's class implementation arrived in 2020 as a strictly
intra-module, no-inheritance prototype, then was filled out in
roughly half-year increments.

- **2020-04-05 commit `6eadb8e77` — *Add simplistic class support
  with many restrictions* (issue #377).** First time `class … endclass`
  parses and elaborates; no inheritance, no virtual methods, no
  parameterised classes. This commit predates the modern PR-squash
  workflow, so the upstream record has no PR number.

- **2020-08-23 commit `20206b1e2` — *Support simple class extends*,
  followed by `3d073c953` *use virtual destruction*.** The minimal
  inheritance footing.

- **PR #3789 — *Support `super.new` calls* (2022-11-30,
  `073dc03ad`).** UVM `build_phase` chains `super.build_phase(phase)`
  through every component layer; without this the UVM base library
  cannot elaborate.

- **PR #4146 — *Support parameterized class references in extends*
  (2023-04-24, `ee5c0a290`)**, and **PR #4616 — *Fix virtual methods*
  (2023-10-24, `84125d7c9`).** Together these unlock the `uvm_object`
  / `uvm_component` parameterised-base-class pattern (`extends
  uvm_sequence #(REQ, RSP)`) that pervades every UVM component.

- **PR #7105 — *Fix `new <obj>` shallow copy not preserving
  polymorphic runtime type* (2026-02-22, `79e1f3317`).** A subtle but
  cv32e40p-relevant bug: `uvm_factory::create_object_by_type` returns
  a base handle but the dynamically-created object has to retain its
  derived RTTI for the subsequent cast in `uvm_create_obj`. Before
  #7105 Verilator's `new_copy` lost the dynamic class, causing UVM
  factory overrides to silently fall back to the base type.

- **PR #7403 — *Fix virtual class inheritance* (2026-04-10,
  `a8f62703a`).** Closes the long-standing gap on abstract base
  classes (`virtual class`), which UVM uses for `uvm_void`,
  `uvm_policy`, etc.

## Stage 3 — Constrained Randomization

This is the densest area of post-2024 work and the one that most
directly tracks the user's "industrial pressure-test" timeline.

- **2020-12-07 commit `cf7ea06b5` — *Support `randomize()` class
  method and rand*.** First-time support, single-variable, no
  constraints.

- **PR #4947 — *Support constrained randomization with external
  solvers* (2024-05-17, `739be2f78`).** The watershed PR: Verilator
  begins emitting SMT-LIB to an external solver (default Z3) instead
  of doing rejection sampling. Every later constraint feature in
  Stage 3 is layered on top of this.

- **PR #5217 — *State-dependent constraints* (2024-07-01,
  `85356f464`)** and **PR #5234 — *Support inline constraints for
  class randomization methods* (2024-07-12)**. The two halves of
  `randomize() with { … }`.

- **PR #5431 — *Support basic dist constraints* (2024-09-12,
  `0b7510bef`)** and **PR #5448 — *Support inside array constraints*
  (2024-09-19)**. RISC-V opcode-distribution generators (the heart of
  every cv32e40p instruction-stream test) need both.

- **PR #5338 — *Support `constraint_mode`* (2024-08-21,
  `930f35acc`)** and the parallel `rand_mode` work in late 2025.

- **PR #7123 — *Support `solve…before` constraints* (2026-02-22,
  `1717df02611`).** The original ticket #5647 sat open for ~18
  months; closing it is a precondition for any UVM constraint-graph
  with ordered legalisation (e.g. RISC-V "pick `xlen` before picking
  `imm` width").

- **PR #7166 — *Support soft constraint solving with last-wins
  priority* (2026-03-01, `108d209bd`)**, plus **PR #7271 — *Fix soft
  constraint relaxation dropping compatible constraints* (2026-03-18,
  `4b34bfffc`).** Soft constraints are the standard idiom for default
  legal values that user-tests can override; without correct relax
  semantics every overridden default leaks into the solver.

- **PR #7066 — *Fix `randomize()` on null object handle crashing
  instead of returning 0* (2026-02-16, `4357aee09`).** The exact
  symptom the user describes: a `uvm_sequence_item` factory miss
  returns `null`, the test then calls `.randomize()` on it expecting a
  benign 0, and pre-#7066 Verilator core-dumped instead.

Beyond these, a long tail of cv32e40p-shape fixes lands in 2026:
#7170 (constraints on fixed arrays of class objects), #7234 (derived
class calling `this.randomize()` with inherited rand members), #7263
(array reduction in constraints crashing with class inheritance),
#7421/#7456 (constraint corruption around queue operands).

## Stage 4 — Interface Polymorphism (Virtual Interfaces)

UVM agents are entirely built on `virtual interface` handles bound
late by the `uvm_config_db`. Verilator's road here is shorter than
randomization, but the bug surface around dynamic binding is just as
deep.

- **PR #3654 — *Support virtual interfaces* (2022-10-20,
  `0e4da3b0b`).** First-time support; the file
  `src/V3SchedVirtIface.cpp` arrives a year later in PR #4673.

- **PR #3674 — *Support clocking blocks* (2022-12-23, `bb44d4e4f`)**
  and **PR #5235 — *Support clocking blocks in virtual interfaces*
  (2024-07-09, `2cfec0ecc`).** Clocking blocks are how UVM monitors
  isolate themselves from race conditions on the DUT-driver
  interface; both PRs are required to drop the cv32e40p `core_if`.

- **PR #4775 — *Support invoking interface methods on virtual
  interface variables* (2023-12-21, `56d679120`).** `vif.driver_task()`
  is the UVM 1.2 idiom for monitor-side hooks.

- **PR #6148 — *Support member-level triggers for virtual
  interfaces* (2025-07-11, `1044398f9`)** and the follow-up reopen
  **PR #6613** (2025-10-29). Without per-member triggering, every
  signal change on the interface re-evaluates *every* observer, which
  scales quadratically with agent count and makes large UVM
  environments unusable.

- **PR #6245 — *Support unassigned virtual interfaces* (2025-08-01,
  `61f4c97f4`)** and **PR #6338 — *Fix broken support of unassigned
  virtual interfaces* (2025-08-28).** Critical for the UVM idiom of
  declaring a `virtual_if` field in `uvm_env` and assigning it via
  `uvm_config_db::get` later in `connect_phase`.

- **PR #7363 — *Fix virtual interface function calls binding to
  wrong instance* (2026-04-02, `1e5c93cc5`).** A textbook polymorphism
  bug: when the same UVM agent class is instantiated twice with two
  different `virtual_if` handles, pre-#7363 Verilator dispatched both
  to whichever was bound last.

## Stage 5 — SystemVerilog Assertions

SVA is Verilator's youngest UVM-relevant feature area: the
combinational-style `assert(expr)` has been around since 2018, but
*concurrent* assertions with sequence/property semantics did not
materially work until 2026.

- **2018-03-11 commit `c8cf2afb1` — *Support assert properties,
  bug785, bug1290*.** A precursor that supported only `assert
  property( @(clk) p )` with `p` a bare boolean expression.

- **PR #3569 — *Support `$sampled`* (2022-08-29, `24ec84851`)**
  and **PR #3572 — *Support negated properties* (2022-08-30,
  `2136afde6`).** First steps toward sampled-value semantics.

- **PR #3667 — *Support named properties* (2022-11-01,
  `bac98df46`).**

- **2025-07-03 commit `f77af4e6f` — *Change `--assert` to be the
  default* and `5a6d5ed96` *Support property `iff` and `implies`*.**
  Inflection point: assertions move from opt-in to default-on, and
  the basic property combinators land.

- **PR #6639 — *Support multi-expression sequences* (2025-11-06,
  `5adecb9fa`)** and **PR #6131 — *Support randsequence*
  (2025-11-30, `b9b6eb61d`).**

- **PR #6944 — *Support procedural concurrent assertion simple
  cases* (2026-03-05, `b89d29cab`).** The first time
  `assert property` is allowed inside `always_ff` blocks, which is
  how RISC-V protocol checkers (e.g. cv32e40p's RVFI assertions)
  are written.

- **PR #7283 — *Support named sequence declarations and instances
  in assertions* (2026-03-20, `a8bccab8e`)** and **PR #7286 —
  *Support property-local variables and sequence match items*
  (2026-03-22, `921607fd3`).**

- **PR #7311/#7312/#7374/#7378/#7392/#7397** (2026-03-30 →
  2026-04-09): consecutive repetition `[*N]` / `[*N:M]` / `[+]`,
  range cycle delay `##[M:N]`, sequence `intersect`, sequence
  `throughout`, `first_match`, nonconsecutive repetition `[=N]`. This
  is the core SVA operator surface needed for the cv32e40p
  protocol-monitor library.

- **PR #7430 — *Use NFA in SVA pass — V3AssertNfa: NFA-based
  multi-cycle SVA evaluation engine* (2026-04-20, `935b25646`).**
  The capstone PR. Replaces the previous DFA/PExpr-visitor lowering
  with a Thompson-NFA evaluation engine in `src/V3AssertNfa.cpp`,
  which is what unlocks bounded-liveness operators (`s_until`,
  `s_eventually`, `s_always[m:n]`) and arbitrary range delays
  without state-space blow-up.

- **PR #7482 — *Support `always` / `always[m:n]` / `s_always[m:n]`
  property operators* (2026-04-27, `c8893b64d`)** and **PR #7506 —
  *Fix internal error on multi-cycle SVA under default clocking*
  (2026-04-28, `bb1bfabab`).** The closing-out fixes for that
  capstone work, both <2 weeks before the head of the local checkout
  (2026-05-01).

---

## Cross-stage observations

1. **The 2022 inflection.** PR #3384 (scheduler) and PR #3363
   (timing) ship within three months of each other and are the
   joint precondition for *every* later UVM-relevant feature. Any
   paper reasoning about the timeline should anchor here, not at
   the older 2020 class-support commits.

2. **The 2024-09 → 2025-Q3 randomization crunch.** The CSV
   shows roughly **209 randomization-related commits across the
   harvest window**, with a clearly visible density spike between
   PR #4947 (May 2024, SMT solver) and PR #5611 (Nov 2024, size
   constraints) — exactly the window the user identifies as their
   cv32e40p ramp-up.

3. **Virtual-interface bugs trail virtual-interface support by
   ~2 years.** First-class support is 2022-Q4 (#3654), but the
   *correctness* PRs (#6148, #6338, #7363) cluster in 2025-Q3 →
   2026-Q2, well after the user's UVM environment started
   exercising them. This is consistent with the user's observation
   that the polymorphism issues only surfaced under industrial-scale
   UVM workloads.

4. **SVA is a 2026 phenomenon.** Of the 119 SVA-tagged commits in
   `05_sva.csv`, 84 (~70%) carry a 2026 timestamp, and 35 of those
   are in March/April 2026 alone — the NFA-engine sprint. Any
   academic claim about Verilator SVA support should explicitly
   bracket pre- vs. post-#7430, because the semantic guarantees
   differ substantially.

---

## How to reproduce / extend

```bash
# from the verilator checkout root, with full history fetched:
git fetch --unshallow origin   # one-time, if originally a shallow clone
cd uvm_pr_harvest
./harvest.sh                    # offline; uses local git log
# or, for canonical PR titles + bodies from GitHub:
GITHUB_TOKEN=ghp_xxx ./harvest_via_gh.py
```

Both scripts emit one CSV per stage in `out/` (or `out_gh/`). For a
filtered subset — e.g. only "Support …" PRs between two dates —
post-process with `awk -F, '$2 ~ /^2024/ && $5 ~ /Support/'` against
`out/all_stages.csv`.
