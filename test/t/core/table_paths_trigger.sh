#!/usr/bin/env bash
# The trigger of sift.table_paths is safe to run again with the rest of the 6.0 SQL, and it takes the lock it needs on the table at once: two sessions that change the paths of two columns of a table,
# one of which has read the table already, wait for each other's transaction and never deadlock on a lock that one of them raised.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init tp 4 "autovacuum = off"
node_start tp || bail "start failed"
sql tp "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, doc jsonb, doc2 jsonb) USING sift_table" "INSERT INTO t SELECT g, jsonb_build_object('k', g), jsonb_build_object('k', g) FROM generate_series(1, 100) g" \
	"SELECT sift.table_set_paths('t', 'doc', 'doc.k int4')" "SELECT sift.table_set_paths('t', 'doc2', 'doc2.k int4')" >/dev/null || bail "setup failed"
D=$(dir tp)

sed "s,MODULE_PATHNAME,\$libdir/pg_sift,g" "$SIFT_REPO/sql/6.0/35-table-paths-trigger.sql" >"$D/part35.sql"
check "the part that makes the trigger runs again without error" psql -X -q -v ON_ERROR_STOP=1 -h "$D/sock" -p "$(port tp)" -U postgres -d postgres -f "$D/part35.sql"
check "the trigger is still on the table, once" test "$(sql tp "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'sift.table_paths'::regclass AND tgname = 'table_paths_trg'")" == 1

CHANGE_A="UPDATE sift.table_paths SET spec = 'doc.k int4' WHERE relid = 't'::regclass AND attname = 'doc'"
CHANGE_B="UPDATE sift.table_paths SET spec = 'doc2.k int4' WHERE relid = 't'::regclass AND attname = 'doc2'"
sess_open tp ra; sess_open tp rb
sess_say ra "SET application_name = 'ra'; BEGIN; SELECT count(*) FROM t; \\echo done"
sess_say rb "SET application_name = 'rb';"
wait_for "grep -q '^done\$' $D/ra.out && [[ \$(sess_count tp rb 'true') == 1 ]]" 60
sess_say rb "$CHANGE_B; \\echo done"
wait_for "[[ \$(sess_count tp rb \"wait_event_type = 'Lock'\") == 1 ]]" 60
sess_say ra "$CHANGE_A; COMMIT; \\echo done"
wait_for "[[ \$(grep -c '^done\$' $D/ra.out) == 2 && \$(grep -c '^done\$' $D/rb.out) == 1 ]]" 30
sess_close ra; sess_close rb
out=$(cat "$D/ra.out" "$D/rb.out")
check "the session that read the table changes its paths while the other waits: no deadlock" test "$(grep -c 'deadlock' <<<"$out")" == 0
check "both changes ran" test "$(grep -c '^done$' <<<"$out")" == 3
