#!/usr/bin/env python3
"""rtlfast oracle: build a design with two Verilator trees and compare them.

  lockstep.py check    --design D.json --ref-root R --dut-root T --work W [--cycles N]
  lockstep.py perf     --design D.json --ref-root R --dut-root T --work W [--cycles N] [--repeat K]
  lockstep.py activity --design D.json --ref-root R --work W [--cycles N]

check     Builds both models, runs them as separate processes with identical
          stimulus, and compares a 64-bit state digest after every cycle plus the
          design's own text output. On a mismatch it reports the first failing
          cycle and the state members that differ there.
perf      Runs both binaries without probing and reports the median speedup.
activity  Reports the mean fraction of state members that change per cycle.

Every run writes a verdict JSON into the work directory. The verdict records
the sha256 of every oracle file, so a result produced by a modified oracle is
recognisable.
"""

import argparse
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ORACLE_FILES = ["harness.cpp", "gen_probes.py", "vlhdr.py", "lockstep.py"]
MIN_COVERAGE = 0.95


def sh(cmd, cwd=None, env=None, log=None, check=True):
    t = time.time()
    with open(log, "w") if log else open(os.devnull, "w") as lf:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=lf, stderr=subprocess.STDOUT)
    if check and p.returncode != 0:
        tail = open(log).read()[-3000:] if log else ""
        sys.exit(f"rtlfast: command failed ({p.returncode}): {' '.join(cmd)}\n{tail}")
    return time.time() - t


def oracle_digest():
    out = {}
    for f in ORACLE_FILES:
        with open(os.path.join(HERE, f), "rb") as fh:
            out[f] = hashlib.sha256(fh.read()).hexdigest()
    return out


def tree_rev(root):
    try:
        return subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True,
                              text=True).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def verilate(root, design, design_dir, mdir, gendir, extra, jobs, log_prefix, sources_key="sources"):
    env = dict(os.environ, VERILATOR_ROOT=root)
    if os.path.exists(mdir):
        shutil.rmtree(mdir)
    srcs = [os.path.join(design_dir, s) for s in design.get(sources_key, design["sources"])]
    cmd = [os.path.join(root, "bin", "verilator"), "--cc", "--exe", os.path.join(HERE, "harness.cpp"),
           "--prefix", "Vm", "--top-module", design["top"], "-Mdir", mdir,
           "--x-assign", "0", "--x-initial", "0", "--output-groups", "0", "-Wno-fatal", "-Wno-lint", "-Wno-style",
           "-CFLAGS", f"-I{gendir}", "-CFLAGS", "-O2", "-j", str(jobs)]
    cmd += design.get("verilator_flags", []) + extra + srcs
    t = sh(cmd, env=env, log=log_prefix + "-verilate.log")
    return t


def make(root, mdir, jobs, log_prefix):
    env = dict(os.environ, VERILATOR_ROOT=root)
    cmd = ["make", "-C", mdir, "-f", "Vm.mk", f"-j{jobs}", "OPT_FAST=-O2", "OPT_SLOW=-O1",
           "OPT_GLOBAL=-O2", "VM_PARALLEL_BUILDS=1"]
    return sh(cmd, env=env, log=log_prefix + "-make.log")


def run_bin(mdir, args, log):
    t = time.time()
    with open(log, "w") as lf:
        p = subprocess.run([os.path.join(mdir, "Vm")] + args, cwd=mdir, stdout=lf,
                           stderr=subprocess.STDOUT)
    return p.returncode, time.time() - t


def design_text(log):
    """The design's own output, without harness bookkeeping lines."""
    keep = []
    with open(log, errors="replace") as f:
        for line in f:
            if line.startswith(("RTLFAST_", "rtlfast:")):
                continue
            keep.append(line)
    return "".join(keep)


def build_pair(a, design, design_dir, need_dut=True):
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    gendir = os.path.join(work, "gen")
    os.makedirs(gendir, exist_ok=True)
    times = {}
    ref_m = os.path.join(work, "ref")
    dut_m = os.path.join(work, "dut")
    times["ref_verilate_s"] = verilate(a.ref_root, design, design_dir, ref_m, gendir,
                                       design.get("ref_flags", []), a.jobs, os.path.join(work, "ref"))
    if need_dut:
        times["dut_verilate_s"] = verilate(a.dut_root, design, design_dir, dut_m, gendir,
                                           design.get("dut_flags", []) + a.dut_flag, a.jobs,
                                           os.path.join(work, "dut"), sources_key="dut_sources")
    else:
        dut_m = ref_m
    sh([sys.executable, "-I", os.path.join(HERE, "gen_probes.py"), "--ref", ref_m, "--dut", dut_m,
        "--design", a.design, "--out", gendir], log=os.path.join(work, "gen.log"))
    times["ref_make_s"] = make(a.ref_root, ref_m, a.jobs, os.path.join(work, "ref"))
    if need_dut:
        times["dut_make_s"] = make(a.dut_root, dut_m, a.jobs, os.path.join(work, "dut"))
    with open(os.path.join(gendir, "coverage.json")) as f:
        cov = json.load(f)
    return work, ref_m, dut_m, cov, times


def cmd_check(a, design, design_dir):
    work, ref_m, dut_m, cov, times = build_pair(a, design, design_dir)
    common = ["--cycles", str(a.cycles), "--reset-cycles", str(design.get("reset_cycles", 10)),
              "--seed", str(a.seed), "--big-every", str(a.big_every)]
    rr, ref_t = run_bin(ref_m, ["--mode", "digest", "--out", os.path.join(work, "ref.dig")] + common,
                        os.path.join(work, "ref-run.log"))
    dr, dut_t = run_bin(dut_m, ["--mode", "digest", "--out", os.path.join(work, "dut.dig")] + common,
                        os.path.join(work, "dut-run.log"))
    a_d = open(os.path.join(work, "ref.dig"), "rb").read()
    b_d = open(os.path.join(work, "dut.dig"), "rb").read()
    first = None
    for i in range(0, min(len(a_d), len(b_d)), 8):
        if a_d[i:i + 8] != b_d[i:i + 8]:
            first = i // 8
            break
    if first is None and len(a_d) != len(b_d):
        first = min(len(a_d), len(b_d)) // 8
    mism = []
    if first is not None:
        dump = ["--mode", "dump", "--dump-cycle", str(first)] + common
        run_bin(ref_m, dump + ["--out", os.path.join(work, "ref.dump")], os.path.join(work, "ref-dump.log"))
        run_bin(dut_m, dump + ["--out", os.path.join(work, "dut.dump")], os.path.join(work, "dut-dump.log"))
        ra = dict(l.split() for l in open(os.path.join(work, "ref.dump")))
        rb = dict(l.split() for l in open(os.path.join(work, "dut.dump")))
        mism = [k for k in ra if rb.get(k) != ra[k]]
    text_equal = design_text(os.path.join(work, "ref-run.log")) == \
        design_text(os.path.join(work, "dut-run.log"))
    ok = (first is None and text_equal and rr == 0 and dr == 0 and cov["coverage"] >= MIN_COVERAGE)
    verdict = {
        "kind": "check", "design": design["name"], "cycles": a.cycles, "seed": a.seed,
        "verdict": "PASS" if ok else "FAIL",
        "digest_equal": first is None, "first_mismatch_cycle": first,
        "mismatched_members_first50": mism[:50], "mismatched_member_count": len(mism),
        "design_text_equal": text_equal, "ref_exit": rr, "dut_exit": dr,
        "coverage": cov, "min_coverage": MIN_COVERAGE,
        "ref_root_rev": tree_rev(a.ref_root), "dut_root_rev": tree_rev(a.dut_root),
        "dut_flags": design.get("dut_flags", []) + a.dut_flag,
        "build_times_s": times, "run_times_s": {"ref": ref_t, "dut": dut_t},
        "oracle_sha256": oracle_digest(),
    }
    out = os.path.join(work, "verdict-check.json")
    json.dump(verdict, open(out, "w"), indent=1)
    print(json.dumps({k: verdict[k] for k in ("verdict", "design", "digest_equal",
                                               "first_mismatch_cycle", "mismatched_member_count",
                                               "design_text_equal")}))
    print(f"rtlfast: coverage={cov['coverage']:.4f} verdict written to {out}")
    return 0 if ok else 1


def cmd_perf(a, design, design_dir):
    work, ref_m, dut_m, cov, times = build_pair(a, design, design_dir)
    args = ["--mode", "perf", "--cycles", str(a.cycles), "--reset-cycles",
            str(design.get("reset_cycles", 10)), "--seed", str(a.seed)]
    res = {"ref": [], "dut": []}
    for k in range(a.repeat):
        for side, mdir in (("ref", ref_m), ("dut", dut_m)):
            log = os.path.join(work, f"{side}-perf{k}.log")
            run_bin(mdir, args, log)
            for line in open(log):
                if line.startswith("RTLFAST_PERF"):
                    res[side].append(float(line.split("cycles_per_sec=")[1]))
    med = {s: statistics.median(v) for s, v in res.items() if v}
    spread = {s: (max(v) - min(v)) / statistics.median(v) for s, v in res.items() if v}
    # A speedup only counts when the two sample ranges do not overlap.
    if res["ref"] and res["dut"]:
        if min(res["dut"]) > max(res["ref"]):
            significance = "faster"
        elif max(res["dut"]) < min(res["ref"]):
            significance = "slower"
        else:
            significance = "within_noise"
    else:
        significance = "no_data"
    verdict = {
        "kind": "perf", "design": design["name"], "cycles": a.cycles, "repeat": a.repeat,
        "cycles_per_sec": res, "median": med,
        "speedup": med["dut"] / med["ref"] if med.get("ref") else None,
        "relative_spread": spread, "significance": significance,
        "ref_root_rev": tree_rev(a.ref_root), "dut_root_rev": tree_rev(a.dut_root),
        "dut_flags": design.get("dut_flags", []) + a.dut_flag,
        "build_times_s": times, "oracle_sha256": oracle_digest(),
        "note": "a speedup only counts together with a PASS check verdict for the same revisions",
    }
    out = os.path.join(work, "verdict-perf.json")
    json.dump(verdict, open(out, "w"), indent=1)
    print(f"rtlfast: median ref={med.get('ref', 0):.1f} dut={med.get('dut', 0):.1f} "
          f"speedup={verdict['speedup'] or 0:.3f} significance={significance} -> {out}")
    return 0


def cmd_activity(a, design, design_dir):
    a.dut_root = a.ref_root
    work, ref_m, _, cov, times = build_pair(a, design, design_dir, need_dut=False)
    log = os.path.join(work, "activity.log")
    run_bin(ref_m, ["--mode", "activity", "--cycles", str(a.cycles), "--reset-cycles",
                    str(design.get("reset_cycles", 10)), "--seed", str(a.seed)], log)
    line = [l for l in open(log) if l.startswith("RTLFAST_ACTIVITY")]
    print(line[0].strip() if line else "rtlfast: no activity line, see " + log)
    verdict = {"kind": "activity", "design": design["name"], "cycles": a.cycles,
               "result": line[0].strip() if line else None, "build_times_s": times,
               "ref_root_rev": tree_rev(a.ref_root), "oracle_sha256": oracle_digest()}
    json.dump(verdict, open(os.path.join(work, "verdict-activity.json"), "w"), indent=1)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["check", "perf", "activity"])
    ap.add_argument("--design", required=True)
    ap.add_argument("--ref-root", required=True)
    ap.add_argument("--dut-root")
    ap.add_argument("--work", required=True)
    ap.add_argument("--cycles", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--big-every", type=int, default=1000)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--dut-flag", action="append", default=[],
                    help="extra Verilator flag for the DUT build (repeatable)")
    a = ap.parse_args()
    if a.cmd != "activity" and not a.dut_root:
        ap.error("--dut-root is required")
    a.ref_root = os.path.abspath(a.ref_root)
    if a.dut_root:
        a.dut_root = os.path.abspath(a.dut_root)
    with open(a.design) as f:
        design = json.load(f)
    design_dir = os.path.dirname(os.path.abspath(a.design))
    return {"check": cmd_check, "perf": cmd_perf, "activity": cmd_activity}[a.cmd](a, design, design_dir)


if __name__ == "__main__":
    sys.exit(main())
