#!/bin/bash
# Development helper (not part of the oracle): build one benchmark with the
# oracle harness against a given Verilator tree, for profiling.
#   build.sh <verilator_root> <design.json> <outdir> [extra verilator flags...]
# Produces <outdir>/obj/Vm. Run e.g.
#   <outdir>/obj/Vm --mode perf --cycles 100000
#   valgrind --tool=callgrind <outdir>/obj/Vm --mode perf --cycles 20000
# Env: OPT (default -O2), JOBS (default nproc), HARNESS (default oracle/harness.cpp;
# tools/prof_harness.cpp with VERILATOR_RTLFAST_PROFILE=<map file> dumps activity counters).
set -euo pipefail
ROOT="$(cd "$1" && pwd)"; DESIGN="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; OUT="$3"; shift 3
K="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-$(nproc)}"
OPT="${OPT:--O2}"
mkdir -p "$OUT/gen"
OUT="$(cd "$OUT" && pwd)"
DDIR="$(dirname "$DESIGN")"
mapfile -t SRCS < <(python3 -c "import json,sys;d=json.load(open('$DESIGN'));print('\n'.join('$DDIR/'+s for s in d['sources']))")
mapfile -t DFLAGS < <(python3 -c "import json;d=json.load(open('$DESIGN'));print('\n'.join(d.get('verilator_flags',[])))")
TOP="$(python3 -c "import json;print(json.load(open('$DESIGN'))['top'])")"
rm -rf "$OUT/obj"
# Run from the output directory: Verilator writes relative VPATH entries, and
# make would pick up a stale harness.o from another build reachable through them
cd "$OUT"
start=$(date +%s.%N)
VERILATOR_ROOT="$ROOT" "$ROOT/bin/verilator" --cc --exe "${HARNESS:-$K/oracle/harness.cpp}" --prefix Vm --top-module "$TOP" \
  -Mdir "$OUT/obj" --x-assign 0 --x-initial 0 --output-groups 0 -Wno-fatal -Wno-lint -Wno-style \
  -CFLAGS "-I$OUT/gen" -CFLAGS "$OPT" "${DFLAGS[@]}" "$@" "${SRCS[@]}" > "$OUT/verilate.log" 2>&1
mid=$(date +%s.%N)
python3 "$K/oracle/gen_probes.py" --ref "$OUT/obj" --dut "$OUT/obj" --design "$DESIGN" --out "$OUT/gen" > "$OUT/gen.log"
VERILATOR_ROOT="$ROOT" make -C "$OUT/obj" -f Vm.mk -j"$JOBS" OPT_FAST="$OPT" OPT_SLOW=-O1 OPT_GLOBAL=-O2 \
  VM_PARALLEL_BUILDS=1 > "$OUT/make.log" 2>&1
end=$(date +%s.%N)
grep -qE "^g\+\+ +$(basename "${HARNESS:-harness.cpp}" .cpp).o " "$OUT/make.log" || { echo "build.sh: harness.o not linked from $OUT/obj" >&2; exit 1; }
python3 -c "print('build.sh: verilate %.1f s, make %.1f s' % ($mid-$start, $end-$mid))"
