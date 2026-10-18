#!/usr/bin/env bash
# pg_sift reference results for the MariaDB correctness check (mariadb/test.sh):
#   pg_ref.sh QUERIES [DATABASE]      QUERIES: lines "id TAB siftql query" (COPY text); the libpq environment selects the server
# Prints, per query, "id TAB count TAB pk:score,..." (top 10 by BM25, ties by ctid): the lines mariadb/tools/compare.py reads.
# The table is docs(id, body) with a sift index on body (bench/sql/schema.sql).
set -euo pipefail
queries=$1 db=${2:-bench} tie=${TIE:+, id}   # TIE=1: ties by id instead of by ctid (the order a MariaDB table has)
{
  cat <<'SQL'
\set ON_ERROR_STOP 1
SET extra_float_digits = 1;
CREATE TEMP TABLE q (id int, q text);
COPY q FROM STDIN;
SQL
  cat "$queries"
  cat <<'SQL'
\.
CREATE FUNCTION pg_temp.ref(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE c bigint; t text;
BEGIN
	BEGIN
		SELECT count(*) INTO c FROM docs WHERE body @@> q;
		SELECT string_agg(id::text || ':' || score::text, ',') INTO t
		FROM (SELECT id, sift.score(ctid) AS score FROM docs WHERE body @@> q ORDER BY sift.score(ctid) DESC LIMIT 10) s;
	EXCEPTION WHEN OTHERS THEN
		RETURN 'error' || E'\t' || SQLERRM;
	END;
	RETURN c || E'\t' || coalesce(t, '');
END $$;
SELECT id || E'\t' || pg_temp.ref(q) FROM q ORDER BY id;
SQL
} | sed "s/ORDER BY sift.score(ctid) DESC LIMIT 10/ORDER BY sift.score(ctid) DESC${tie} LIMIT 10/" | psql -X -A -t -d "$db"
