#!/usr/bin/env bash
# sift.maintain of a Format 1.7 BM25 index beside TRUNCATE of its table: the call locks the table before the index, in the order of TRUNCATE, so a TRUNCATE that comes while the call stops
# at the injection point sift-maint-before-table-lock (PostgreSQL built with --enable-injection-points: scripts/pg-assert.sh --inject) waits for it, and both end with no deadlock.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }
((PG_MAJOR >= 18)) || { echo "1..0 # SKIP the wait of an injection point is named from PostgreSQL 18"; exit 0; }

plan 5
node_init ml 3 "autovacuum = off
sift.async_maintenance = off
deadlock_timeout = 100ms"
node_start ml || bail "start failed"
D=$(dir ml)
P=sift-maint-before-table-lock
sql ml "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" "CREATE TABLE facts (id serial, body text)" "CREATE INDEX facts_idx ON facts USING sift (body)" \
	"INSERT INTO facts (body) SELECT 'word' || g || ' common' FROM generate_series(1, 200) g" >/dev/null || bail "setup failed"
check "the index is Format 1" test "$(sql ml "SELECT (sift.index_format('facts_idx')).major")" == 1

count() { sql ml "SELECT count(*) FROM pg_stat_activity WHERE $1"; }
sql ml "SELECT injection_points_attach('$P', 'wait')" >/dev/null
sql ml "SELECT busy FROM sift.maintain('facts_idx')" >"$D/maintain.out" 2>&1 & mp=$!
wait_for "[[ \$(count \"wait_event = '$P'\") == 1 ]]" 60
check "sift.maintain stops before it locks the table" test "$(count "wait_event = '$P'")" == 1
sql ml "TRUNCATE facts" >"$D/truncate.out" 2>&1 & tp=$!
wait_for "[[ \$(count \"query = 'TRUNCATE facts' AND wait_event_type = 'Lock'\") == 1 ]]" 60
sql ml "SELECT injection_points_detach('$P')" "SELECT injection_points_wakeup('$P')" >/dev/null
wait "$mp"
check "sift.maintain ends" test $? == 0
wait "$tp"
check "TRUNCATE ends" test $? == 0
check "no deadlock, no error" test "$(grep -cE 'deadlock detected|ERROR|TRAP' "$D/postgres.log" "$D/maintain.out" "$D/truncate.out" | awk -F: '{ s += $NF } END { print s }')" == 0
finish
