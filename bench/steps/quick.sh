#!/usr/bin/env bash
# Usage: bench/steps/quick.sh [--full] [ENGINE...]
# Standard suite in one command: Q tier (150k articles), 36 fixed queries in 8 classes (term, and,
# or, phrase, prefix, fuzzy, count(*), ranked mix), 8 clients x REPEAT (50) requests per class after
# 5 warm-up requests each, fixed seeds. Under 1 min per engine including the index build. Prints the
# table (bench/steps/report.sh quick) and keeps raw rows in bench/out/<tier>.
# --full: the timed workloads of run.sh on the F tier (218k articles), 30 s each (about 10 min per engine), with
# the 8 vCPU / 32 GB budget of bench/steps/lib.sh.
# ENGINE defaults to: gin rum pg_textsearch bm25 pg_search sift tantivy elasticsearch.
set -euo pipefail
full=; [[ ${1:-} == --full ]] && { full=1; shift; }
export TIER=${TIER:-$([[ $full ]] && echo F || echo Q)}
. "$(dirname "$0")/lib.sh"

mkdir -p "$OUT"
"$STEPS/cluster.sh" init
"$STEPS/cluster.sh" start
db_exists bench || "$STEPS/corpus.sh" restore
"$STEPS/machine.sh" >"$OUT/machine.txt"

wls=quick
[[ $full ]] && wls=
for e in ${*:-gin rum pg_textsearch bm25 pg_search sift tantivy elasticsearch}; do
  t0=$(date +%s) load0=$(loadavg)
  "$STEPS/$(runner "$e")" "$e" $wls
  printf '%s\t%s\t%s\t%s\n' "$TIER" "$e" $(( $(date +%s) - t0 )) "$load0" >>"$OUT/walltime.tsv"
done
"$STEPS/report.sh" quick "$OUT"
