# rtlfast kit

A self-driving setup for one job: make Verilator much faster on large
synchronous SystemVerilog RTL without ever changing results. An agent runs
unattended on the Claude Platform (Managed Agents) against a fork of Verilator;
a lockstep oracle decides what counts as correct.

```
rtlfast/                 lives in the fork, branch "rtlfast"
  MISSION.md             the agent's charter (canonical copy lives in a read-only memory store)
  GATE.json              designs, cycles and seeds every accepted change must pass
  BASE_COMMIT            upstream commit the reference Verilator is built from
  oracle/                lockstep judge: two builds, separate processes, per-cycle state digest
  bench/                 open benchmark configs; fetch.sh downloads the designs
  scripts/               bootstrap, gate, push, unlock
  notes/                 STATE, LEDGER, REPORT (Chinese), ORACLE_ISSUES, GATE_PROPOSALS
  results/               verdict JSON files committed with each change
rtlfast/  platform/
    SYSTEM.md            agent system prompt
    rtlfast_platform.py  creates environment, agent, memory stores, vault, deployment; run/pause/status
```

## The oracle in one paragraph

`oracle/lockstep.py check` verilates the same design twice, once with stock
Verilator at `BASE_COMMIT` and once with the fork, compiles each into its own
binary with the same harness, and runs both with identical stimulus. After
every cycle each binary hashes every design state member that exists under the
same name in both models (scalars masked to their declared width) plus all
outputs; the digest streams and the designs' own text output must be identical.
On a mismatch it reports the first failing cycle and the members that differ.
Coverage below 0.95 fails the check, so a backend cannot pass by hiding state.

## Local use

```
python3 rtlfast/oracle/lockstep.py check --design rtlfast/bench/rocket/design.json \
  --ref-root /path/to/stock/verilator --dut-root /path/to/fork --work /tmp/w --cycles 200000
python3 rtlfast/oracle/lockstep.py perf  ... --repeat 3
python3 rtlfast/oracle/lockstep.py activity --design ... --ref-root ... --work /tmp/a
```
