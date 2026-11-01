#!/bin/sh
# Runs db_bench for each engine RUNS times; raw output goes to bench/results/.
set -eu
RUNS=${RUNS:-5}
NUM=${NUM:-1000000}
OUT=${OUT:-bench/results}
DB=${DB:-/tmp/sediment-bench}
mkdir -p "$OUT"
: > "$OUT/db_bench_raw.txt"
{
  echo "# $(date '+%Y-%m-%d %H:%M') $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(sw_vers -productVersion 2>/dev/null || uname -r)"
  echo "# $(c++ --version | head -1); leveldb $(brew list --versions leveldb 2>/dev/null | cut -d' ' -f2); sqlite $(sqlite3 --version | cut -d' ' -f1)"
  echo "# num=$NUM value_size=100 runs=$RUNS db=$DB"
} >> "$OUT/db_bench_raw.txt"
for run in $(seq 1 "$RUNS"); do
  for engine in sediment leveldb sqlite; do
    ./build/db_bench --engine=$engine --num="$NUM" --db="$DB-$engine" | sed "s/^/run$run /" >> "$OUT/db_bench_raw.txt"
  done
done
python3 scripts/bench_table.py "$OUT/db_bench_raw.txt" > "$OUT/db_bench.md"
cat "$OUT/db_bench.md"
