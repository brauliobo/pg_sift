#!/usr/bin/env bash
# Usage: bench/quick.sh [--full] [ENGINE...]
# Standard suite in one command: Q tier (25k articles), 36 fixed queries in 8 classes (term, and,
# or, phrase, prefix, fuzzy, count(*), ranked mix), 8 clients x REPEAT (50) requests per class after
# 5 warm-up requests each, fixed seeds. Under 1 min per engine on S including the index build. Prints the
# table (bench/report.sh quick) and keeps raw rows in bench/out/<tier>.
# --full: the TIN-style workloads on the F tier (218k articles), 30 s each (about 10 min per engine).
# ENGINE defaults to: gin rum pg_textsearch bm25 paradedb sift tantivy elasticsearch.
set -euo pipefail
full=; [[ ${1:-} == --full ]] && { full=1; shift; }
export TIER=${TIER:-$([[ $full ]] && echo F || echo Q)}
. "$(dirname "$0")/lib.sh"

mkdir -p "$OUT"
"$BENCH/cluster.sh" init
"$BENCH/cluster.sh" start
[[ $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_database WHERE datname = 'bench'") == 1 ]] || "$BENCH/corpus.sh" restore
printf 'host %s | %s | %s | %s\n' "$(uname -n)" "$(grep -m1 -oP 'model name\s+: \K.*' /proc/cpuinfo)" \
  "$(grep -qw avx512f /proc/cpuinfo && echo AVX-512 || echo 'AVX2, no AVX-512')" "$(date -u +%FT%TZ)" >"$OUT/host.txt"

wls=quick
[[ $full ]] && wls=
for e in ${*:-gin rum pg_textsearch bm25 paradedb sift tantivy elasticsearch}; do
  t0=$(date +%s) load0=$(cut -d' ' -f1 /proc/loadavg)
  if grep -q '^EXTERNAL=' "$BENCH/engines/$e.sh"; then "$BENCH/xrun.sh" "$e" $wls; else "$BENCH/run.sh" "$e" $wls; fi
  printf '%s\t%s\t%s\t%s\n' "$TIER" "$e" $(( $(date +%s) - t0 )) "$load0" >>"$OUT/walltime.tsv"
done
"$BENCH/report.sh" quick "$OUT"
