#!/usr/bin/env bash
# BM25 on a heap table, branch against reference: proves the table AM branch left the heap paths of the sift index alone. Both servers
# get the same 200000-document heap table and sift index; bench/ab-query.sh then times a matching count and a ranked top-10 on each,
# interleaved.
#
#   bench/tbl-ab.sh NEW_SOCKDIR:PORT REF_SOCKDIR:PORT      (clusters from scripts/pgtest.sh, user postgres)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
(( $# == 2 )) || { sed -n '2,8p' "$0"; exit 1; }
for side in "$1" "$2"; do
	host=${side%%:*} port=${side#*:}
	psql -X -qAt -h "$host" -p "$port" -U postgres -d postgres -c "DROP DATABASE IF EXISTS bench WITH (FORCE)" -c "CREATE DATABASE bench"
	psql -X -qAt -h "$host" -p "$port" -U postgres -d bench <<'SQL'
CREATE EXTENSION pg_sift;
SELECT setseed(0.5);
CREATE TABLE docs AS SELECT g AS id, concat_ws(' ', 'w' || g % 7, 'w' || g % 101, 'w' || g % 1009, 'doc' || g, CASE WHEN g % 5 = 0 THEN 'rare' END) AS body
	FROM generate_series(1, 200000) g;
CREATE INDEX docs_bm25 ON docs USING sift (body);
VACUUM ANALYZE docs;
SQL
done
for q in "SELECT count(*) FROM docs WHERE body @@> 'w1 AND w100'" "SELECT id FROM docs WHERE body @@> 'w1 OR rare' ORDER BY sift.score(ctid) DESC LIMIT 10"; do
	echo "== $q"
	"$REPO/bench/ab-query.sh" -T 3 -r 7 -s 'SET enable_seqscan = off' "$q" "$1/bench" "$2/bench"
done
