#!/usr/bin/env sh
# Generates the synthetic circuits (once) and runs the benchmark on them plus any
# ISCAS'85 files found in bench/iscas/. Build first:
#   cmake -S . -B build -G Ninja && cmake --build build
# usage: sh bench/run_bench.sh [build-dir] [runs]
set -eu

BUILD=${1:-build}
RUNS=${2:-10}
GEN=bench/generated
mkdir -p "$GEN"

gen() {  # gen <name> <layers> <width>
    if [ ! -f "$GEN/$1.bench" ]; then
        "$BUILD/gen_random_dag" --layers "$2" --width "$3" --seed 1 --out "$GEN/$1.bench"
    fi
}
gen synth_10k 100 100       #  10,000 gates
gen synth_100k 200 500      # 100,000 gates
gen synth_1m 1000 1000      # 1,000,000 gates

set -- bench/c17.bench bench/s27.bench
# ISCAS circuits (see bench/iscas/README.md), c* before s*, numerically ordered.
for f in $(ls bench/iscas/*.bench 2>/dev/null | sort -V); do
    [ -f "$f" ] && set -- "$@" "$f"
done
set -- "$@" "$GEN/synth_10k.bench" "$GEN/synth_100k.bench" "$GEN/synth_1m.bench"

"$BUILD/gatetime_bench" --runs "$RUNS" --top 10 "$@"
