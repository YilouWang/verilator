#!/bin/bash
# Run the gate defined in rtlfast/GATE.json against the current DUT build.
#   gate.sh check   correctness on every gate design (required for any commit)
#   gate.sh perf    speed on the perf designs (only meaningful after check PASS)
# Verdicts go to rtlfast/results/<UTC date>-<short sha>/.
set -uo pipefail
REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
K="$REPO/rtlfast"
REF="${RTLFAST_REF:-/workspace/verilator-ref}"
WORK="${RTLFAST_WORK:-/workspace/rtlfast-work}"
MODE="${1:-check}"
SHA="$(git -C "$REPO" rev-parse --short HEAD)"
DIRTY=""; git -C "$REPO" diff --quiet -- src include || DIRTY="-dirty"
OUT="$K/results/$(date -u +%Y%m%d)-$SHA$DIRTY"
mkdir -p "$OUT"
DUTFLAG=()
if grep -qE '"-?rtl-fast"' "$REPO/src/V3Options.cpp" 2>/dev/null; then DUTFLAG=(--dut-flag=--rtl-fast); fi

status=0
python3 - "$K/GATE.json" "$MODE" > "$WORK/gate-plan.txt" <<'EOF'
import json, sys
g = json.load(open(sys.argv[1]))
for item in g[sys.argv[2]]:
    for seed in item.get("seeds", [1]):
        print(item["design"], item["cycles"], seed, item.get("repeat", 3))
EOF
while read -r design cycles seed repeat; do
  name="$(python3 -c "import json;print(json.load(open('$K/$design'))['name'])")"
  w="$WORK/$MODE-$name-s$seed"
  if [ "$MODE" = check ]; then
    python3 -I "$K/oracle/lockstep.py" check --design "$K/$design" --ref-root "$REF" --dut-root "$REPO" \
      --work "$w" --cycles "$cycles" --seed "$seed" "${DUTFLAG[@]}" || status=1
    cp "$w/verdict-check.json" "$OUT/check-$name-s$seed.json" 2>/dev/null || status=1
  else
    python3 -I "$K/oracle/lockstep.py" perf --design "$K/$design" --ref-root "$REF" --dut-root "$REPO" \
      --work "$w" --cycles "$cycles" --repeat "$repeat" "${DUTFLAG[@]}" || status=1
    cp "$w/verdict-perf.json" "$OUT/perf-$name.json" 2>/dev/null || status=1
  fi
done < "$WORK/gate-plan.txt"
echo "rtlfast: gate $MODE $([ $status = 0 ] && echo PASS || echo FAIL) -> $OUT"
exit $status
