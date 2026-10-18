#!/usr/bin/env bash
# Usage: [COPIES=n] TIER=Q|F bench/steps/build.sh [WORKERS...]    (default: 0 2 4; 0 = serial build)
# Timed sift CREATE INDEX per parallel worker count, on a clone of the restored `bench` database
# (bench/steps/cluster.sh start; bench/steps/corpus.sh restore). The table is vacuumed and prewarmed before each
# build, which holds the perf lock. Appends tier, workers, seconds, index MB, segments, load average to
# $OUT/build-workers.tsv. COPIES=n builds on n copies of the corpus (a table big enough for long parallel scan chunks).
. "$(dirname "$0")/lib.sh"

load_engine sift
mkdir -p "$OUT"
EDB=e_build_workers
PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $EDB WITH (FORCE)" -c "CREATE DATABASE $EDB TEMPLATE bench STRATEGY FILE_COPY"
export PGDATABASE=$EDB
COPIES=${COPIES:-1}
q -c "$SETUP"
(( COPIES == 1 )) || q -c "INSERT INTO docs SELECT d.id + g * (SELECT max(id) FROM docs), d.body, d.category_id,
                             d.created, d.likes, d.url FROM docs d, generate_series(1, $((COPIES - 1))) g"
q -c "VACUUM (ANALYZE) docs"

prewarm() {
  q -c "SELECT sum(pg_prewarm(c.oid)) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname IN ('public', 'pg_toast') AND c.relkind IN ('r', 't')" >/dev/null
}

# The table's parallel_workers fixes the worker count (else it follows the table size); CREATE INDEX takes
# at most max_parallel_maintenance_workers of them.
timed_build() {
  local t0; t0=$(now_ms)
  q -c "ALTER TABLE docs SET (parallel_workers = $1)" -c "SET max_parallel_maintenance_workers = $1" \
    -c "SET maintenance_work_mem = '${TIER_MWM[$TIER]}'" -c "$INDEX" >/dev/null
  echo $(( $(now_ms) - t0 )) >"$OUT/build-workers.ms"
}

for w in ${*:-0 2 4}; do
  q -c "DROP INDEX IF EXISTS docs_fts"
  prewarm
  load=$(loadavg)
  batch "build, $w workers"
  locked timed_build "$w"
  printf '%s\t%s\t%.1f\t%s\t%s\t%s\n' "$TIER$([[ $COPIES == 1 ]] || echo "x$COPIES")" "$w" "$(cat "$OUT/build-workers.ms")e-3" \
    "$(q -c "SELECT round(pg_relation_size('docs_fts') / 1e6)")" "$(q -c "SELECT count(*) FROM sift.segments('docs_fts')")" \
    "$load" | tee -a "$OUT/build-workers.tsv"
done
rm -f "$OUT/build-workers.ms"
PGDATABASE=postgres q -c "DROP DATABASE $EDB WITH (FORCE)"
