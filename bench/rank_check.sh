#!/usr/bin/env bash
# Usage: TIER=Q|F bench/rank_check.sh ENGINE [NSUB]
# Soft BM25 sanity check: overlap of the engine's top-10 with pgx-bm25's top-10 for the first
# NSUB (default 200) substrings, per mode. Ties at rank 10 keep even exact BM25 below 1.0.
# The pgx-bm25 reference is computed once per tier into $DATA/rankref-$TIER.tsv.
. "$(dirname "$0")/lib.sh"

NSUB=${2:-200}
REF=$DATA/rankref-$TIER-$NSUB.tsv

top10() { q -c "COPY (SELECT q.id, bench_ids(bench_stmt('top10', q.mode, q.terms)) FROM queries q
                      WHERE q.sub <= $NSUB AND bench_rank(q.mode, q.terms) IS NOT NULL ORDER BY q.id) TO STDOUT"; }

if [[ ! -s $REF ]]; then
  load_engine bm25
  check_db
  top10 >"$REF.tmp" && mv "$REF.tmp" "$REF"
fi
load_engine "${1:?engine}"
engine_available || { log "$ENGINE: extension $REQUIRES not installed, skipping"; exit 0; }
check_db
mkdir -p "$OUT"
top10 | psql -X -v ON_ERROR_STOP=1 -q -c "CREATE TEMP TABLE got (id int, ids bigint[]); CREATE TEMP TABLE ref (id int, ids bigint[])" \
  -c "\\copy got FROM STDIN" -c "\\copy ref FROM '$REF'" \
  -c "SELECT '$ENGINE' AS engine, q.mode, count(*) AS queries,
             round(avg(cardinality(ARRAY(SELECT unnest(g.ids) INTERSECT SELECT unnest(r.ids)))::numeric
                       / nullif(cardinality(r.ids), 0)), 3) AS overlap_at_10,
             count(*) FILTER (WHERE cardinality(g.ids) <> cardinality(r.ids)) AS size_differs
      FROM got g JOIN ref r USING (id) JOIN queries q USING (id) GROUP BY q.mode ORDER BY q.mode" | tee "$OUT/rank-$ENGINE.txt"
