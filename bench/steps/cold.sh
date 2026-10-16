#!/usr/bin/env bash
# Usage: TIER=F [REPS=3] [GUC=sift.prefetch] bench/steps/cold.sh
# Cold-cache latency of the quick suite's queries (ranked top 10 per class; unordered sum() over all matches)
# with a boolean GUC off and on, on a clone of the restored `bench` database. Before every run the index and
# the heap are evicted from shared_buffers (pg_buffercache_evict_relation) and from the OS page cache
# (posix_fadvise DONTNEED on their files), so each run reads from the device. Appends
# per query median ms (off, on) to $OUT/cold-queries.tsv and per class sums to $OUT/cold.tsv. Holds the perf lock for the whole run.
. "$(dirname "$0")/lib.sh"

load_engine sift
mkdir -p "$OUT"
EDB=e_cold REPS=${REPS:-3} GUC=${GUC:-sift.prefetch}
PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $EDB WITH (FORCE)" -c "CREATE DATABASE $EDB TEMPLATE bench STRATEGY FILE_COPY"
export PGDATABASE=$EDB
q -c "$SETUP" -c "CREATE EXTENSION IF NOT EXISTS pg_buffercache" -c "$RENDER" -c "$INDEX" -c "VACUUM (FREEZE, ANALYZE) docs" -c "CHECKPOINT"

datadir=$(q -c "SHOW data_directory")
files=$(q -c "SELECT string_agg(pg_relation_filepath(c), ' ') FROM unnest(ARRAY['docs', 'docs_fts']::regclass[]) c")
cold() {
  q -c "SELECT pg_buffercache_evict_relation(c) FROM unnest(ARRAY['docs', 'docs_fts']::regclass[]) c" >/dev/null
  python3 - "$datadir" $files <<'EOF'
import glob, os, sys
for rel in sys.argv[2:]:
    for f in glob.glob(os.path.join(sys.argv[1], rel) + '*'):
        fd = os.open(f, os.O_RDONLY)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        os.close(fd)
EOF
}

# One query, cold, with the GUC at $3; prints the execution time in ms
run() {
  local sql=$1 kind=$2 mode=$3 t
  cold
  t=$(q -c "SET $GUC = $mode" -c "EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, COSTS OFF) $sql" | sed -n 's/^Execution Time: \([0-9.]*\) ms/\1/p')
  printf '%s\t%s\t%s\n' "$kind" "$mode" "$t"
}

queries() {
  q -f "$BENCH/sql/quick.sql" -c "SELECT class || '.' || sub, sift_q(mode, terms) FROM quick WHERE kind = 'top10' AND class <> 'mix' ORDER BY class, sub"
}

body() {
  local class qry
  while IFS='|' read -r class qry; do
    qry=${qry//\'/\'\'}
    for rep in $(seq "$REPS"); do
      for mode in off on; do
        printf '%s\t' "$class"
        run "SELECT id FROM docs WHERE body @@> '$qry' ORDER BY sift.score(ctid) DESC LIMIT 10" ranked "$mode"
        [[ $class =~ ^(term|and|or)\. ]] || continue
        printf '%s\t' "$class"
        run "SELECT sum(a_count) FROM docs WHERE body @@> '$qry'" unordered "$mode"
      done
    done
  done < <(queries) | tee "$OUT/cold-raw.tsv" >/dev/null
}

locked body
# per query: median off and on, then per class the sums (the unit a class is judged by)
awk -F'\t' -v tier="$TIER" '{ v[$2 FS $1 FS $3] = v[$2 FS $1 FS $3] " " $4 }
  END { for (k in v) { n = split(v[k], a, " "); asort(a); print k FS a[int((n + 1) / 2)] } }' "$OUT/cold-raw.tsv" |
  sort | awk -F'\t' -v tier="$TIER" '{ m[$1 FS $2 FS $3] = $4; q[$1 FS $2] = 1 }
    END { for (k in q) { split(k, f, FS); c = f[2]; sub(/\..*/, "", c); o = m[k FS "off"]; n = m[k FS "on"]
      printf "%s\t%s\t%s\t%s\t%.2f\t%.2f\n", tier, f[1], f[2], c, o, n } }' | sort | tee -a "$OUT/cold-queries.tsv" |
  awk -F'\t' -v tier="$TIER" '{ k = $2 FS $4; o[k] += $5; n[k] += $6 }
    END { for (k in o) printf "%s\t%s\t%.1f\t%.1f\t%+.0f%%\n", tier, k, o[k], n[k], (n[k] / o[k] - 1) * 100 }' | sort | tee -a "$OUT/cold.tsv"
PGDATABASE=postgres q -c "DROP DATABASE $EDB WITH (FORCE)"
