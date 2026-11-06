#!/bin/bash
# The first BM25 queries of a backend that stem phrases through a text search configuration, in a correlated subquery (one rescan of the scan for each phrase):
# the stem memo of the backend is made while the scan is at its first row, and the recheck of the phrases after the first rescan reset the context that
# had taken the place of the recheck context the rescan deleted (a crash: lang_config at the first statement of a backend, in the plain suite it is
# never the first). The setup is that of test/sql/lang_config.sql up to its first comparison; each query is a session of its own.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

plan 4
node_init cs 29 "autovacuum = off"
node_start cs || bail "start failed"
D=$(dir cs)
# the file has statements that fail by design, so no ON_ERROR_STOP
sed -n '1,/^SELECT dm_answers() = dm_ref_answers() AS batches_same;/p' "$REPO/test/sql/lang_config.sql" | sed '$d' |
	psql -X -q -At -h "$D/sock" -p "$(port cs)" -U postgres -d postgres >/dev/null 2>&1
[[ $(sql cs "SELECT count(*) FROM dm") -gt 0 ]] || bail "setup failed"

PHRASES="ARRAY['\"Hund und Katze\"', '\"Katze die Straße\"', '\"Kinder laufen\"', 'Haus', '\"Hund und Katze\"']"
IDX="SELECT string_agg(md5(r), ' ' ORDER BY n) FROM (SELECT n, q, (SELECT string_agg(id::text, ',' ORDER BY id) FROM dm WHERE body @@> q) AS r FROM unnest($PHRASES) WITH ORDINALITY u(q, n)) x"
HEAP="SELECT string_agg(md5(r), ' ' ORDER BY n) FROM (SELECT n, q, (SELECT string_agg(id::text, ',' ORDER BY id) FROM dm WHERE to_tsvector('german', body) @@ phraseto_tsquery('german', btrim(q, '\"'))) AS r FROM unnest($PHRASES) WITH ORDINALITY u(q, n)) x"
want=$(sql cs "SET enable_indexscan = off" "SET enable_bitmapscan = off" "$HEAP" | tail -1)
for i in 1 2 3; do got=$(sql cs "$IDX" 2>&1 | tail -1); [[ $got == "$want" ]] || break; done
check "the phrases of a correlated subquery answer as the heap does, in a new session each time" test "$got" == "$want" -a -n "$want"
check "the SQL function of lang_config (eight queries) agrees with its reference in a new session" test "$(sql cs "SELECT dm_answers() = dm_ref_answers()" 2>&1 | tail -1)" == t
check "no backend was terminated by a signal" test "$(grep -c 'terminated by signal' "$D/postgres.log")" == 0
check "the server is up" test "$(sql cs "SELECT 1")" == 1
finish
