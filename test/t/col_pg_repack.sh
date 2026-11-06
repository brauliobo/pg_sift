#!/usr/bin/env bash
# pg_repack of a heap table with a sift_col and a sift (BM25) index, with writers running: the copy is built with the index definitions, the log of the
# writers is applied to it, the swap changes every ctid, and the rebuilt indexes answer as the heap does. The sift_table that pg_repack would destroy is refused.
# Skipped where the pg_repack binary and extension are not installed (postgresql-NN-repack).
. "$(dirname "$0")/lib.bash"

plan 24
command -v pg_repack >/dev/null || { skip 24 "pg_repack is not installed"; finish; }
node_init rp 28 "autovacuum = off"
node_start rp || bail "start failed"
sql rp "CREATE EXTENSION pg_sift CASCADE" >/dev/null || bail "pg_sift"
sql rp "CREATE EXTENSION pg_repack" >/dev/null 2>&1 || { skip 24 "the pg_repack extension is not available"; finish; }
sql rp "CREATE TABLE facts (id int PRIMARY KEY, grp int, kind text, amount numeric(10,2), title text, doc jsonb, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, (g * 7919) % 200, 'cat_' || (g % 8), g % 997, 'alpha beta ' || (g % 50), jsonb_build_object('score', g % 11), repeat('x', 100 + g % 400) FROM generate_series(1, 40000) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, grp, kind, amount, doc) WITH (paths = 'doc.score float8')" \
	"CREATE INDEX facts_bm ON facts USING sift (title)" "VACUUM facts" \
	"CREATE TABLE st (id int PRIMARY KEY, v int) USING sift_table" "INSERT INTO st SELECT g, g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
D=$(dir rp)
repack() { pg_repack -h "$D/sock" -p "$(port rp)" -U postgres -d postgres "$@"; }
state_of() { sql rp "SELECT concat_ws(',', track_changes, triggers, complete, heap_free) FROM sift.col_tracking('$1')"; }
# the answers of the column scan (the normal plan priced out) and of the heap, as one string
SCAN="SELECT kind, grp % 5 g, count(*), sum(amount), sum((doc->>'score')::float8) FROM facts GROUP BY 1, 2"
answers() {
	sql rp "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 1000" "SET sift.enable_agg = $1" \
		"SELECT string_agg(x::text, ',' ORDER BY x::text) FROM ($SCAN) x" | tail -1
}
defs() { sql rp "SELECT string_agg(regexp_replace(pg_get_indexdef(indexrelid), 'index_[0-9]+', 'index'), E'\n' ORDER BY indexrelid::regclass::text) FROM pg_index WHERE indrelid = 'facts'::regclass AND indexrelid::regclass::text IN ('facts_col', 'facts_bm')"; }
points() { sql rp "SELECT round(point_units) FROM sift.col_clustering('facts_col') WHERE path = 'grp'"; }

before_defs=$(defs)
before_points=$(points)
# writer BASE: the keys of its inserts start at BASE, so that two runs do not meet
writer() {
	for i in $(seq 1 25); do
		sql rp "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $i" "DELETE FROM facts WHERE id % 100 = $i + 50 AND id < 40000" \
			"INSERT INTO facts SELECT ${1:-100000} + $i * 1000 + g, g % 200, 'cat_' || (g % 8), g % 997, 'alpha beta ' || (g % 50), jsonb_build_object('score', g % 11), 'y' FROM generate_series(1, 200) g" \
			>/dev/null 2>>"$D/writer.err" || echo failed >>"$D/writer.err"
	done
}
: >"$D/writer.err"
writer & wp=$!
repack -t facts -o "grp, id" >"$D/repack.log" 2>&1
check "pg_repack -o grp, id finished beside the writer" test "$?" == 0 -a -z "$(grep -E 'ERROR|WARNING' "$D/repack.log")"
wait "$wp"
check "the writer's transactions all committed" test ! -s "$D/writer.err"
check "the rebuilt sift_col and sift indexes have the definitions of the old ones" test "$(defs)" == "$before_defs"
check "the sift_col index verifies against the heap" test -z "$(sql rp "SELECT sift.col_verify('facts_col', true)" 2>&1)"
check "the BM25 index verifies against the heap" test -z "$(sql rp "SELECT sift.verify('facts_bm', true)" 2>&1)"
check "the column scan is the plan" test "$(sql rp "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 1000" "EXPLAIN (COSTS OFF) $SCAN" | grep -c SiftAgg)" == 1
check "the column scan answers as the heap does" test "$(answers on)" == "$(answers off)"
check "the BM25 index finds the rows the heap has" test "$(sql rp "SELECT count(*) FROM facts WHERE title @@> 'alpha beta 7'")" == "$(sql rp "SELECT count(*) FROM facts WHERE title LIKE 'alpha beta 7'")"
check "the new order tightens the zones of grp (units an equality touches: $before_points before)" test "$(points)" -lt "$before_points"
check "the rewrite left the visibility map empty (the doc tells to VACUUM)" test "$(sql rp "SELECT relallvisible FROM pg_class WHERE oid = 'facts'::regclass")" == 0
# an aggressive VACUUM waits for a page another backend has pinned (the bgwriter writing the rewritten heap, say); a plain one skips it and leaves it out of the map
sql rp "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) facts" >/dev/null
# the copy is a full rewrite under a snapshot and the writers of the run were applied to it by statements, so its log is complete: the triggers of the table are the ones that keep it so
check "the rebuilt sift_col index tracks its changes again (track_changes, triggers, complete, heap_free)" test "$(sql rp "SELECT concat_ws(',', track_changes, triggers, complete, heap_free) FROM sift.col_tracking('facts_col')")" == "t,t,t,t"
check "a grouped read of the rebuilt index is a SiftAgg scan that visits no heap page" test "$(sql rp "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 1000" "SET sift.enable_agg = on" \
	"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $SCAN" | grep -cE 'SiftAgg|Heap Pages' | tr '\n' ' ')" == "1 "
sql rp "DELETE FROM facts WHERE id % 97 = 3" "UPDATE facts SET id = id + 500000 WHERE id % 89 = 5" >/dev/null
check "rows deleted and keys changed after the rewrite are heard by the triggers of the table: the column scan answers as the heap does" test "$(answers on)" == "$(answers off)" -a "$(sql rp "SELECT heap_free FROM sift.col_tracking('facts_col')")" == t
check "after VACUUM the visibility map is set again" test "$(sql rp "SELECT relallvisible = relpages AND relpages > 0 FROM pg_class WHERE oid = 'facts'::regclass")" == t
# --only-indexes names the copy of the index in the schema of the table (public.index_<oid>), not in repack: the copy takes the triggers of the index by the swap, none of its own
# (dropping them with the copy needs ACCESS EXCLUSIVE on the table, which the writers queued behind it deadlock with)
d0=$(grep -c 'deadlock detected' "$D/postgres.log")
: >"$D/writer.err"
writer 300000 & wp=$!
repack -t facts --only-indexes >"$D/repack2.log" 2>&1
check "pg_repack --only-indexes finished beside the writer, and the writer's transactions all committed" test "$?" == 0 -a -z "$(grep -E 'ERROR|WARNING' "$D/repack2.log")" -a ! -s "$D/writer.err"
wait "$wp"
check "... no deadlock was detected, no copy is left, and the table has the two triggers of each index" \
	test "$(($(grep -c 'deadlock detected' "$D/postgres.log") - d0))|$(sql rp "SELECT count(*) FROM pg_class WHERE relname LIKE 'index\_%'")|$(sql rp "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'facts'::regclass AND tgname LIKE 'sift_track_%'")" == "0|0|2"
# the copy was built beside the writers, so its log is not complete (flags 0): the reads check the heap and answer as it does until the next VACUUM re-baselines it
check "... the index answers as the heap does" test "$(answers on)" == "$(answers off)" -a "$(sql rp "SELECT track_changes FROM sift.col_tracking('facts_col')")" == t
sql rp "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) facts" >/dev/null
check "... and tracks its changes again after the VACUUM (track_changes, triggers, complete, heap_free)" test "$(sql rp "SELECT concat_ws(',', track_changes, triggers, complete, heap_free) FROM sift.col_tracking('facts_col')")" == "t,t,t,t"
check "pg_repack refuses the sift_table and leaves it readable" test "$(repack -t st 2>&1 | grep -c 'cannot rewrite the sift_table')|$(sql rp "SELECT count(*), sum(v) FROM st")" == "1|1000|500500"
check "pg_repack --only-indexes of the sift_table is fine" test "$(repack -t st --only-indexes >/dev/null 2>&1; echo $?)|$(sql rp "SELECT count(*) FROM st")" == "0|1000"
# A pg_repack that dies while it builds the sift_col copy (here: killed, its backend ends the statement it was in) leaves what it committed: its log table, the trigger of pg_repack on the table, the copy and the copy's
# indexes. The copy owns the two triggers of its own index, on the copy table that nobody writes, so they never fire; the table keeps the two triggers of its index and its reads. What costs the writers
# is the trigger of pg_repack, which logs every change until the extension is dropped (pg_repack says so when it is run again). The copy is held in its index build by an event trigger.
sql rp "CREATE FUNCTION hold_copy() RETURNS event_trigger LANGUAGE plpgsql AS \$\$ BEGIN IF current_query() ~ '^CREATE INDEX index_[0-9]+ ON repack\\.table_[0-9]+ USING sift_col' THEN PERFORM pg_sleep(3); END IF; END \$\$" \
	"CREATE EVENT TRIGGER hold_copy ON ddl_command_start EXECUTE FUNCTION hold_copy()" >/dev/null || bail "the event trigger"
pg_repack -h "$D/sock" -p "$(port rp)" -U postgres -d postgres -t facts >"$D/repack4.log" 2>&1 & rp4=$!
for _ in $(seq 1 400); do
	[[ $(sql rp "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'pg_repack' AND wait_event = 'PgSleep'") -gt 0 ]] && break
	sleep 0.05
done
kill -9 "$rp4"; wait "$rp4" 2>/dev/null
# the statement the backend is in ends and commits (a CREATE INDEX is a transaction of its own), then the backend finds its client gone
for _ in $(seq 1 200); do [[ $(sql rp "SELECT count(*) FROM pg_stat_activity WHERE application_name = 'pg_repack'") == 0 ]] && break; sleep 0.1; done
sql rp "DROP EVENT TRIGGER hold_copy" >/dev/null
trig_on() { sql rp "SELECT count(*) FROM pg_trigger WHERE tgrelid::regclass::text = '$1' AND tgname LIKE '$2'"; }
ct=$(sql rp "SELECT c.relname FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'repack' AND c.relname ~ '^table_'")
check "the killed pg_repack leaves its copy ($ct), whose sift_col index owns two triggers of its own on the copy table, and the table keeps the two of facts_col" \
	test -n "$ct" -a "$(trig_on "repack.$ct" 'sift_track_%')" == 2 -a "$(trig_on facts 'sift_track_%')" == 2
: >"$D/writer.err"
writer 700000
check "writes to the table go on beside the leftover (the triggers of facts_col fire, the copy's never), and the reads answer as the heap, heap-free" \
	test ! -s "$D/writer.err" -a "$(state_of facts_col)" == "t,t,t,t" -a "$(answers on)" == "$(answers off)"
check "DROP EXTENSION pg_repack CASCADE removes the leftover at once (the copy's index and triggers, pg_repack's trigger), and facts_col keeps its triggers and its answers" \
	test "$(sql rp "SET statement_timeout = 20000" "DROP EXTENSION pg_repack CASCADE" >/dev/null 2>&1; echo $?)|$(sql rp "SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'repack'")|$(sql rp "SELECT count(*) FROM pg_trigger WHERE tgname LIKE 'repack%'")|$(trig_on facts 'sift_track_%')" == "0|0|0|2" -a "$(answers on)" == "$(answers off)"
sql rp "CREATE EXTENSION pg_repack" >/dev/null
check "pg_repack runs again after it, and the index tracks its changes after the next VACUUM" test "$(repack -t facts >/dev/null 2>&1; echo $?)" == 0 -a "$(sql rp "VACUUM facts" >/dev/null; state_of facts_col)" == "t,t,t,t" -a "$(answers on)" == "$(answers off)"
finish
