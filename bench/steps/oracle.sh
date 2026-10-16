#!/usr/bin/env bash
# Usage: TIER=Q|F bench/steps/oracle.sh ENGINE [NSUB]
# Checks the first NSUB (default 200) trace substrings as and/or/phrase/not queries: the engine's
# match set and count(*) must equal the oracle's (tsvector, case folding, stemming off).
# Env: ORACLE_ACCENTS=fold|keep (default fold; use keep for gin/rum/bm25, which index raw 'simple')
#      ORACLE_STEM=off|<snowball language> (default off)
#      ORACLE_TOKENS=split|pg (default split: words are alphanumeric runs like sift's; pg for engines using the PG parser: gin, rum)
#      MODES="and or phrase not"
#      REBUILD=1 rebuilds the o_ENGINE database. Exit status 1 on any mismatch.
. "$(dirname "$0")/lib.sh"

load_engine "${1:?engine}"
NSUB=${2:-200}
engine_available || { log "$ENGINE: extension $REQUIRES not installed, skipping"; exit 0; }
mkdir -p "$OUT"
check_db
q -v accents="${ORACLE_ACCENTS:-fold}" -v stem="${ORACLE_STEM:-off}" -v tokens="${ORACLE_TOKENS:-split}" -f "$BENCH/sql/oracle.sql"
REPORT=$OUT/oracle-$ENGINE.txt

psql -X -v ON_ERROR_STOP=1 -q -v nsub="$NSUB" -v modes="${MODES:-and or phrase not}" <<'SQL' | tee "$REPORT"
CREATE TEMP TABLE res AS
SELECT q.id, q.mode, q.terms, r.*
FROM oracle_queries q CROSS JOIN LATERAL oracle_check(q.mode, q.terms) r
WHERE q.sub <= :nsub AND q.mode = ANY (string_to_array(:'modes', ' '));

\echo == summary (accents/stem: see oracle_tsv comment) ==
SELECT obj_description('oracle_tsv'::regclass, 'pg_class') AS oracle_cfg, mode,
       count(n_engine) AS checked, count(*) - count(n_engine) AS unsupported,
       count(*) FILTER (WHERE missing + extra > 0 OR n_count <> n_oracle) AS mismatched,
       sum(n_oracle) AS oracle_rows
FROM res GROUP BY mode ORDER BY mode;

\echo == mismatches ==
SELECT r.id, r.mode, array_to_string(r.terms, ' ') AS query, r.n_oracle, r.n_engine, r.n_count,
       r.missing, r.missing_ids, r.extra, r.extra_ids,
       left(regexp_replace(d.body, '\s+', ' ', 'g'), 160) AS first_diff_doc
FROM res r
LEFT JOIN docs d ON d.id = coalesce(r.missing_ids[1], r.extra_ids[1])
WHERE r.missing + r.extra > 0 OR r.n_count <> r.n_oracle
ORDER BY r.id LIMIT 50;

SELECT count(*) = 0 AS ok FROM res WHERE missing + extra > 0 OR n_count <> n_oracle \gset
\if :ok
\else
\echo FAIL
\endif
SQL
! grep -q '^FAIL$' "$REPORT"
