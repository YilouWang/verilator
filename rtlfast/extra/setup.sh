#!/bin/bash
# Fetch extra open-source designs (outside the gate) at pinned commits and write
# their design.json files for oracle/lockstep.py. Inputs other than clock and
# reset are driven with seeded random data by the oracle harness.
#   setup.sh <dir>     then e.g.
#   python3 -I rtlfast/oracle/lockstep.py check --design <dir>/ibex/design.json \
#     --ref-root <ref> --dut-root <repo> --work <w> --cycles 200000 --seed 1 --dut-flag=--rtl-fast
set -euo pipefail
D="$(mkdir -p "$1" && cd "$1" && pwd)"
fetch() {  # name url sha
  [ -d "$D/src/$1/.git" ] || git clone -q "$2" "$D/src/$1"
  git -C "$D/src/$1" checkout -q "$3"
}
fetch picorv32 https://github.com/YosysHQ/picorv32.git ef203c2b0a3fb793280f5114941416c425c5b461
fetch ibex https://github.com/lowRISC/ibex.git 1b29dae8866e6c0f0a3cd9dead53a77b4f45e357
fetch veer https://github.com/chipsalliance/Cores-VeeR-EL2.git 0169d669eedec31f723dde006125e22c4bf83a95
fetch vexriscv https://github.com/litex-hub/pythondata-cpu-vexriscv.git 642ecfed1c84460555d6d803d660cc60cfc1ecb6
(cd "$D/src/veer" && RV_ROOT="$PWD" perl configs/veer.config -target=default -snapshot=default > /dev/null)
python3 - "$D" <<'PY'
import json, os, sys
D = sys.argv[1]
S = D + "/src"
def write(name, d):
    os.makedirs(f"{D}/{name}", exist_ok=True)
    d = dict(name=name, reset_cycles=10, workload="random inputs", **d)
    json.dump(d, open(f"{D}/{name}/design.json", "w"), indent=1)
write("picorv32", dict(top="picorv32", sources=[f"{S}/picorv32/picorv32.v"], clock="clk",
      reset="resetn", reset_active=0, verilator_flags=[f"-G{p}=1" for p in (
      "ENABLE_MUL", "ENABLE_DIV", "COMPRESSED_ISA", "ENABLE_IRQ", "ENABLE_TRACE",
      "ENABLE_COUNTERS64", "CATCH_MISALIGN", "BARREL_SHIFTER")]))
P, R = f"{S}/ibex/vendor/lowrisc_ip/ip", f"{S}/ibex/rtl"
ibex = dict(top="ibex_top", clock="clk_i", reset="rst_ni", reset_active=0,
    sources=[f"{P}/prim/rtl/{p}.sv" for p in ("prim_util_pkg", "prim_mubi_pkg", "prim_secded_pkg",
             "prim_cipher_pkg", "prim_count_pkg")]
            + [f"{P}/prim_generic/rtl/prim_ram_1p_pkg.sv", f"{R}/ibex_pkg.sv", f"{R}/ibex_cheriot_pkg.sv",
               f"{R}/ibex_top.sv"],
    verilator_flags=["+define+SYNTHESIS", f"+incdir+{P}/prim/rtl",
                     f"+incdir+{S}/ibex/vendor/lowrisc_ip/dv/sv/dv_utils",
                     "-y", R, "-y", f"{P}/prim/rtl", "-y", f"{P}/prim_generic/rtl"])
write("ibex", ibex)
full = dict(ibex)
full["verilator_flags"] = ibex["verilator_flags"] + [f"-G{p}" for p in (
    "ICache=1", "BranchPredictor=1", "WritebackStage=1", "PMPEnable=1", "SecureIbex=1",
    "BranchTargetALU=1", "MHPMCounterNum=4", "DbgTriggerEn=1")]
write("ibex-full", full)
V = f"{S}/veer"
flist = [l.strip().replace("-v ", "").replace("$RV_ROOT", V)
         for l in open(f"{V}/design/flist") if l.strip() and not l.startswith("//")]
write("veer-el2", dict(top="veer_wrapper", clock="clk", reset="rst_l", reset_active=0,
      sources=[f"{V}/snapshots/default/common_defines.vh", f"{V}/design/include/el2_def.sv",
               f"{V}/snapshots/default/el2_pdef.vh"] + flist + [f"{V}/testbench/veer_wrapper.sv"],
      verilator_flags=["+define+RV_OPENSOURCE", f"+incdir+{V}/testbench",
                       f"+incdir+{V}/snapshots/default", f"+incdir+{V}/design/include",
                       f"+incdir+{V}/design/lib"]))
# SpinalHDL marks some signals public, which keeps the core a separate class:
# --flatten puts its state into the root class, where the oracle probes it
for v in ("Linux", "Full"):
    write("vexriscv-" + v.lower(), dict(top="VexRiscv", clock="clk", reset="reset", reset_active=1,
          sources=[f"{S}/vexriscv/pythondata_cpu_vexriscv/verilog/VexRiscv_{v}.v"],
          verilator_flags=["--flatten"]))
print("rtlfast: extra designs ready in", D)
PY
