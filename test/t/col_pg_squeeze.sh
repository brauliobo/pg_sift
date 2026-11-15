#!/usr/bin/env bash
# pg_squeeze of a heap table with a sift_col index that tracks its changes, with a writer and a reader running. The copy (tmp_<oid>, in the schema of the table) is built without the constraints,
# with a sift_col index of its own, and takes the place of the table by swapping files. The writer's changes reach it through logical decoding, by statements that no trigger hears, so the log
# of the index it ends with is complete for nobody: its witnesses name the triggers that went with the copy, the triggers that stay are the table's own. The reads check the heap (right, slower)
# until the next VACUUM re-baselines the log (the maintenance worker and a read that has to check the heap do it as well, col_track_revouch.sh).
# Skipped where pg_squeeze is not installed: SIFT_SQUEEZE_ROOT is the DESTDIR of a `make install` of pg_squeeze (never the system's directories), or it is in the server's own.
. "$(dirname "$0")/lib.bash"

plan 11
SQ=${SIFT_SQUEEZE_ROOT:-}
[[ -e $SQ$(pg_config --pkglibdir)/pg_squeeze$DLSUFFIX ]] || { skip 11 "pg_squeeze is not installed (SIFT_SQUEEZE_ROOT is the DESTDIR of its make install)"; finish; }
node_init sq 29 "autovacuum = off
wal_level = logical
max_replication_slots = 4
max_wal_senders = 4
shared_preload_libraries = 'pg_squeeze'"
node_conf sq "extension_control_path = '$SQ$(pg_config --sharedir):$ROOT$(pg_config --sharedir):\$system'
dynamic_library_path = '$SQ$(pg_config --pkglibdir):$ROOT$(pg_config --pkglibdir):\$libdir'"
D=$(dir sq)
# the minors of PostgreSQL 18 that refuse a library as an output plugin unless it is named here
postgres -C output_plugin_libraries -D "$D/data" >/dev/null 2>&1 && node_conf sq "output_plugin_libraries = 'pg_squeeze'"
node_start sq || bail "start failed"
sql sq "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE EXTENSION pg_squeeze" >/dev/null || bail "extensions"
ROWS=$(knob SIFT_SQ_ROWS 40000 12000)
sql sq "CREATE TABLE facts (id int PRIMARY KEY, grp int NOT NULL, kind text NOT NULL, amount int NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 50, 'cat_' || (g % 8), g % 997, repeat('x', 100 + g % 300) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, grp, kind, amount)" "VACUUM (FREEZE, ANALYZE) facts" \
	"DELETE FROM facts WHERE id % 3 = 0" "UPDATE facts SET amount = amount + 1 WHERE id % 5 = 0" >/dev/null || bail "setup failed"
Q="SELECT string_agg(kind || ':' || n || ':' || s, ' ' ORDER BY kind) FROM (SELECT kind, count(*) n, sum(amount) s FROM facts GROUP BY kind) q"
# ans MODE: free (the heap-free read), checked (the heap checked for the rows that need it), heap (the normal plan)
ans() { sql sq "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $([[ $1 == heap ]] && echo off || echo on)" "SET sift.test_no_heapfree = $([[ $1 == checked ]] && echo on || echo off)" "$Q" | tail -n 1; }
differential() { [[ $(ans free) == $(ans heap) && $(ans checked) == $(ans heap) ]]; }
state() { sql sq "SELECT concat_ws(',', track_changes, triggers, complete, heap_free) FROM sift.col_tracking('facts_col')"; }
state_is() { [[ $(state) == "$1" ]] && differential; }
ntrig() { sql sq "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'facts'::regclass AND tgname LIKE 'sift_track_%'"; }
copies() { sql sq "SELECT count(*) FROM pg_class WHERE relname LIKE 'tmp\_%' OR relname LIKE 'index\_%'"; }
squeeze() { sql sq "SELECT squeeze.squeeze_table('public', 'facts', $1, NULL, NULL)" >"$D/squeeze.out" 2>"$D/squeeze.err"; }
squeezed_clean() { squeeze "$1" && [[ ! -s $D/squeeze.err && "$(copies)|$(ntrig)" == "0|2" ]] && differential; }
# one snapshot reads the table both ways: the index and the heap agree in every snapshot the reader takes
cat >"$D/rr.sql" <<EOF
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET max_parallel_workers_per_gather = 0;
SET sift.enable_agg = on;
$Q;
SET sift.enable_agg = off;
$Q;
COMMIT;
EOF
reader() {
	local end=$((SECONDS + $1)) n=0 bad=0 out
	while ((SECONDS < end)); do
		out=$(psql -X -At -q -h "$D/sock" -p "$(port sq)" -U postgres -d postgres -f "$D/rr.sql" 2>&1 | grep -v '^SET$\|^BEGIN$\|^COMMIT$')
		n=$((n + 1))
		# no output at all is two empty answers, the index's and the heap's: a snapshot older than the swap of the files sees the new file empty, as it does for the heap (a psql error is a line of output)
		[[ $(sed -n 1p <<<"$out") == "$(sed -n 2p <<<"$out")" ]] || { bad=$((bad + 1)); echo "$out" >>"$D/reader.bad"; }
	done
	echo "$n $bad" >"$D/reader.out"
}
writer() {
	for i in $(seq 1 25); do
		sql sq "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $i" "DELETE FROM facts WHERE id % 100 = $i + 50 AND id < $ROWS" \
			"INSERT INTO facts SELECT $1 + $i * 1000 + g, g % 50, 'cat_' || (g % 8), g % 997, 'y' FROM generate_series(1, 200) g" \
			>/dev/null 2>>"$D/writer.err" || echo failed >>"$D/writer.err"
	done
}
check "before: the index tracks its changes (track_changes, triggers, complete, heap_free)" test "$(state)" == "t,t,t,t"
: >"$D/writer.err"
d0=$(grep -c 'deadlock detected' "$D/postgres.log")
writer 500000 & wp=$!
reader 8 & rp=$!
squeeze NULL
rc=$?
wait "$wp" "$rp"
check "pg_squeeze finished beside a writer and a reader" test "$rc" == 0 -a ! -s "$D/squeeze.err"
check "... the writer's transactions all committed, and no deadlock was detected" test ! -s "$D/writer.err" -a "$(($(grep -c 'deadlock detected' "$D/postgres.log") - d0))" -eq 0
check "... the index and the heap gave one answer in every snapshot the reader took ($(cat "$D/reader.out") snapshots, differences)" test "$(cut -d' ' -f2 "$D/reader.out")" == 0 -a "$(cut -d' ' -f1 "$D/reader.out")" -gt 0
check "... no copy of the table or an index is left, and the table has the two triggers of the index" test "$(copies)|$(ntrig)" == "0|2"
check "... the heap-free read, the checked read and the heap give one answer" differential
# the writer's changes reached the copy by logical decoding: no trigger heard them, and the triggers that stay are not the ones the log names
check "the log is not taken for complete (triggers false, heap_free false): the reads check the heap, and answer as it does" state_is t,f,t,f
sql sq "VACUUM facts" >/dev/null
check "the next VACUUM re-baselines it: tracking again, heap-free, and the answers are the heap's" state_is t,t,t,t
: >"$D/writer.err"
writer 900000
quiet_then() { [[ ! -s $D/writer.err ]] && state_is "$1"; }
check "writes after it are heard by the triggers of the table: heap-free, and the answers are the heap's" quiet_then t,t,t,t
# a squeeze that sorts by an index, without a writer
check "pg_squeeze ordered by the primary key finishes, leaves no copy, and the answers are the heap's" squeezed_clean "'facts_pkey'"
sql sq "VACUUM facts" >/dev/null
check "... and the next VACUUM gives the log its baseline again" state_is t,t,t,t
finish
