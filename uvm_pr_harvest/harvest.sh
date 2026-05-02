#!/usr/bin/env bash
# uvm_pr_harvest/harvest.sh
#
# Mine the local Verilator git history for PRs/commits relevant to the
# five UVM-enabling thematic stages, and emit one CSV per stage plus a
# combined CSV. Run from the root of a verilator checkout that has the
# full upstream history (run `git fetch --unshallow origin` first if the
# clone was shallow).
#
# Output columns: stage,date,sha,pr,subject
# `pr` is parsed from the trailing "(#NNNN)" in the commit subject; if
# absent it is left blank (early Verilator commits used "bugNNNN"
# references in the issue tracker, not GitHub PR numbers).
#
# Usage:
#   ./harvest.sh            # writes ./out/*.csv against upstream/master
#   ./harvest.sh main       # use a different base ref
#
# No network calls; pure git log.

set -euo pipefail
REF="${1:-origin/master}"
OUT="$(dirname "$0")/out"
mkdir -p "$OUT"

emit() {
    local stage="$1"; shift
    local pattern="$1"; shift
    local since="$1"; shift
    local until="$1"; shift
    git log "$REF" --no-merges \
        --since="$since" --until="$until" \
        --grep="$pattern" -i --extended-regexp \
        --format='%cI%x09%H%x09%s' \
    | awk -v stage="$stage" 'BEGIN{FS="\t";OFS=","}
        {
            subj=$3
            pr=""
            tmp=subj
            while (match(tmp,/\(#[0-9]+\)/)) {
                hit=substr(tmp,RSTART+2,RLENGTH-3)
                pr=hit
                tmp=substr(tmp,RSTART+RLENGTH)
            }
            gsub(/"/,"\"\"",subj)
            print stage,$1,substr($2,1,12),pr,"\"" subj "\""
        }'
}

# Stage 1: Scheduling & Time Semantics
emit "01_scheduling" \
    'schedul|timing|coroutine|NBA|non-blocking|fork|join|#0 delay|#1step|active region|reactive|preponed|observed|V3Sched|V3Timing|V3Fork|V3Delayed' \
    '2020-01-01' '2026-12-31' > "$OUT/01_scheduling.csv"

# Stage 2: SystemVerilog OOP (Classes & Dynamic Objects)
emit "02_oop" \
    'class extend|extends class|super\.new|virtual method|virtual function|polymorph|inheritance|class member|class typedef|class param|parameterized class|null handle|null object|new\(\)' \
    '2019-01-01' '2026-12-31' > "$OUT/02_oop.csv"

# Stage 3: Constrained Randomization
emit "03_random" \
    'randomize|constraint|rand_mode|constraint_mode|solve.*before|randc|std::randomize|pre_randomize|post_randomize|dist|inside.*constraint|soft constraint|extern constraint|pure constraint' \
    '2020-01-01' '2026-12-31' > "$OUT/03_random.csv"

# Stage 4: Interface Polymorphism (Virtual Interfaces / Modports / Clocking)
emit "04_iface" \
    'virtual interface|virtual modport|virtual iface|VirtIface|modport|clocking block|clocking output|clocking input|clocking ref|interface methods|interface ref|interface array|parameterized interface' \
    '2020-01-01' '2026-12-31' > "$OUT/04_iface.csv"

# Stage 5: SVA Concurrent Assertions
emit "05_sva" \
    'concurrent assert|assert property|cover property|assume property|propert|sequence|sampled value|disable iff|throughout|first_match|until|s_until|nexttime|s_eventually|intersect|goto repetition|consecutive repetition|nonconsecutive repetition|implication|named sequence|local variable|V3Assert|NFA|sva\b' \
    '2018-01-01' '2026-12-31' > "$OUT/05_sva.csv"

# Combined view, header included
{
    echo "stage,date,sha,pr,subject"
    cat "$OUT"/0*.csv
} > "$OUT/all_stages.csv"

echo "Wrote:" "$OUT"/*.csv
wc -l "$OUT"/*.csv
