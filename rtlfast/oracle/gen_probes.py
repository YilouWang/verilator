#!/usr/bin/env python3
"""Generate the harness include files from the reference and DUT models.

Usage: gen_probes.py --ref REF_MDIR --dut DUT_MDIR --design design.json --out GENDIR

The probe list is the intersection of the design state members of both
models, in reference order. Coverage (how much of the reference state the
DUT still exposes under the same names) is written to GENDIR/coverage.json.
The gate in lockstep.py refuses a comparison whose coverage is below the
threshold, so a backend cannot pass by hiding state.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vlhdr  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--dut", required=True)
    ap.add_argument("--design", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    with open(a.design) as f:
        design = json.load(f)
    design_dir = os.path.dirname(os.path.abspath(a.design))

    ref_members = vlhdr.parse_root(os.path.join(a.ref, "Vm___024root.h"))
    dut_members = vlhdr.parse_root(os.path.join(a.dut, "Vm___024root.h"))
    dut_names = {m["name"] for m in dut_members}
    common = [m for m in ref_members if m["name"] in dut_names]
    missing = [m["name"] for m in ref_members if m["name"] not in dut_names]

    ports = vlhdr.parse_ports(os.path.join(a.ref, "Vm.h"))
    dut_ports = {p["name"] for p in vlhdr.parse_ports(os.path.join(a.dut, "Vm.h"))}
    if {p["name"] for p in ports} != dut_ports:
        print("rtlfast: top-level ports differ between ref and dut", file=sys.stderr)
        sys.exit(3)

    clock = design["clock"]
    reset = design["reset"]
    os.makedirs(a.out, exist_ok=True)

    with open(os.path.join(a.out, "probes.inc"), "w") as f:
        for m in common:
            w = m["width"] if (m["kind"] != "unpacked" and m["width"]) else 0
            f.write(f"P({m['name']}, {w})\n")
    with open(os.path.join(a.out, "outputs.inc"), "w") as f:
        for p in ports:
            if p["dir"] != "IN":
                f.write(f"O({p['name']}, {p['width']})\n")
    with open(os.path.join(a.out, "inputs.inc"), "w") as f:
        for p in ports:
            if p["dir"] == "IN" and p["name"] not in (clock, reset) \
                    and p["name"] not in design.get("hold_inputs", {}):
                f.write(f"I({p['name']}, {p['width']})\n")
    with open(os.path.join(a.out, "preload.inc"), "w") as f:
        for pl in design.get("preload", []):
            path = os.path.join(design_dir, pl["file"])
            f.write(f'PRELOAD({pl["member"]}, "{path}")\n')
    with open(os.path.join(a.out, "design.inc"), "w") as f:
        f.write(f"#define CLOCK_PORT {clock}\n")
        f.write(f"#define RESET_PORT {reset}\n")
        f.write(f"#define RESET_ACTIVE {int(design.get('reset_active', 1))}\n")

    cov = {
        "ref_members": len(ref_members),
        "dut_members": len(dut_members),
        "common": len(common),
        "coverage": len(common) / max(1, len(ref_members)),
        "missing_in_dut_first50": missing[:50],
    }
    with open(os.path.join(a.out, "coverage.json"), "w") as f:
        json.dump(cov, f, indent=1)
    print(f"rtlfast: probes={len(common)} ref={len(ref_members)} dut={len(dut_members)} "
          f"coverage={cov['coverage']:.4f}")


if __name__ == "__main__":
    main()
