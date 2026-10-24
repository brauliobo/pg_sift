#!/usr/bin/env bash
# The crash matrix of the operations that publish a new directory of an index: build, write-buffer promotion, merge and VACUUM of a BM25 index and of a
# sift_col index (with the options cube_minmax, order_columns and key_columns, column format minors 7, 13 and 15), the sealing of a sift_table and the promotion of its write buffer by the
# worker. Every publication (store_update) is a named point of the testing build: sift.test_crash_dir = n kills the process with SIGKILL before the
# nth directory update (n > 0) or after it (n < 0), its log flushed, so that the segment is written and the directory still the old one, or the
# directory is the new one and nothing after it ran. For each operation the points are the first and the last of its updates (more in the full
# run). After the restart (wal_consistency_checking = generic: crash recovery compares every page it rebuilds with the page the primary wrote) the
# indexes verify (heapallindexed), the store checks pass, the answers through the indexes equal the plain plan and what the table held before, and
# the operation, run again, completes and leaves it the same. SIFT_CRASHMX_ITERS caps the number of crashes (default 60, 20 quick: the first and the last update of every operation).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_CRASHMX_ITERS 60 20)
FULL=$(knob SIFT_CRASHMX_FULL 1 "")
node_init cm 1 "wal_consistency_checking = 'generic'
autovacuum = off
sift.async_maintenance = off
max_parallel_maintenance_workers = 0
shared_buffers = 64MB
include_if_exists = 'crash.conf'"
node_start cm || bail "start failed"
D=$(dir cm)
SIFT_TMP=$D leak_expect 200 'terminated by signal 9'
sql cm "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

PLAIN=("SET sift.enable_agg = off" "SET sift.enable_scan = off" "SET sift.enable_count = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off")
IDX=("SET sift.enable_agg = on" "SET seq_page_cost = 10000" "SET enable_seqscan = off")
QA="SELECT g, count(*), sum(n), min(id), max(id) FROM docs GROUP BY g ORDER BY g"
QB="SELECT count(*), sum(id) FROM docs WHERE body @@> 'alpha3 beta4'"
QT="SELECT count(*), sum(id), sum(g * 3 + n), count(DISTINCT id), count(DISTINCT n) FROM"
QO="SELECT string_agg(id || ':' || g || ':' || n, ',' ORDER BY g DESC, n, id) FROM (SELECT id, g, n FROM docs ORDER BY g DESC, n, id LIMIT 200) x"
BASE="CREATE TABLE docs (id int, g int, n int, body text) WITH (autovacuum_enabled = off);
	INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 5 || ' beta' || i % 11 FROM generate_series(1, 6000) i"
more() { echo "INSERT INTO docs SELECT i, i % 9, i % 100, 'alpha' || i % 3 || ' beta' || i % 7 FROM generate_series($1, $2) i"; }
BM25="CREATE INDEX docs_idx ON docs USING sift (body) WITH (write_buffer_limit = 4096)"
COL="CREATE INDEX docs_col ON docs USING sift_col (id, g, n) WITH (write_buffer_limit = 4096, cube_minmax = true, order_columns = 'g, n', key_columns = 'g, n')"
TBL="CREATE TABLE tbl (id int, g int, n int) USING sift_table WITH (autovacuum_enabled = false); CREATE TABLE tbl_h (id int, g int, n int)"

# scenario ~ setup (after the base table and its rows) ~ the operation
SCENARIOS=(
	"bm25_build~~$BM25"
	"col_build~~$COL"
	"bm25_promote~$BM25; $(more 6001 8000)~SELECT sift.promote('docs_idx')"
	"col_promote~$COL; $(more 6001 8000)~SELECT sift.col_promote('docs_col')"
	"bm25_merge~$BM25; $(more 6001 7000); SELECT sift.promote('docs_idx'); $(more 7001 8000); SELECT sift.promote('docs_idx')~SELECT sift.merge('docs_idx', true)"
	"col_merge~$COL; $(more 6001 7000); SELECT sift.col_promote('docs_col'); $(more 7001 8000); SELECT sift.col_promote('docs_col')~SELECT sift.col_merge('docs_col', true)"
	"vacuum~$BM25; $COL; DELETE FROM docs WHERE id % 3 = 0; UPDATE docs SET n = n + 1 WHERE id % 7 = 0~VACUUM docs"
	"tbl_seal~$TBL; INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i; INSERT INTO tbl_h SELECT i, i % 9, i % 100 FROM generate_series(1, 9000) i; DELETE FROM tbl WHERE id % 5 = 0; DELETE FROM tbl_h WHERE id % 5 = 0~VACUUM tbl"
)

# healthy: every sift index verifies against its heap and passes the store check
VERIFY="DO \$\$ DECLARE r record; BEGIN
	FOR r IN SELECT c.oid::regclass AS ix, a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE a.amname IN ('sift', 'sift_col') LOOP
		IF r.amname = 'sift' THEN PERFORM sift.verify(r.ix, true); ELSE PERFORM sift.col_verify(r.ix, true); END IF;
		PERFORM sift._test_check(r.ix);
	END LOOP; END \$\$"
# the answers of everything the scenario created, in a mode
answers() {
	local -n mode=$1
	local has qs=("$QA" "$QB")
	has=$(sql cm "SELECT string_agg(relname, ',') FROM pg_class WHERE relname IN ('docs', 'tbl')")
	[[ $has != *docs* ]] || qs+=("$QT docs" "$QO")
	[[ $has != *tbl* ]] || qs+=("$QT tbl" "$QT tbl_h")
	sql cm "${mode[@]}" "${qs[@]}" 2>&1
}
healthy() { # the indexes verify and the answers through them are the plain plan's and REF
	local plain
	sql cm "$VERIFY" >&2 || return 1
	plain=$(answers PLAIN)
	[[ $plain == "$REF" ]] || { diag "the plain answers changed: $plain"; return 1; }
	[[ $(answers IDX) == "$plain" ]] || { diag "the answers through the indexes differ from the plain plan"; return 1; }
	twins=$(sql cm "SELECT count(*) FROM pg_class WHERE relname = 'tbl'")
	[[ $twins == 0 ]] || [[ $(sql cm "$QT tbl") == "$(sql cm "$QT tbl_h")" ]] || { diag "the sift_table differs from its heap twin"; return 1; }
}
fresh() { # SETUP
	sql cm "DROP TABLE IF EXISTS docs, tbl, tbl_h CASCADE" "$BASE" >/dev/null || return 1
	[[ -z $1 ]] || sql cm "$1" >/dev/null || return 1
	REF=$(answers PLAIN)
}
await_exit() { # PID
	for _ in $(seq 600); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.1; done
	return 1
}

total=0
for row in "${SCENARIOS[@]}"; do
	IFS='~' read -r name setup op <<<"$row"
	[[ -z ${SIFT_CRASHMX_ONLY:-} || $name =~ $SIFT_CRASHMX_ONLY ]] || continue
	fresh "$setup" || bail "$name: setup"
	healthy || bail "$name: the starting state is not healthy"
	# the directory updates of the operation, counted in a session of its own
	k=$(sql cm "$op" "SELECT sift._test_dir_updates()" | tail -1)
	(( k > 0 )) || bail "$name: the operation published nothing"
	points=(1 "-$k")
	[[ -z $FULL ]] || points+=(-1 "$k" $(( (k + 1) / 2 )))
	declare -A seen=()
	for n in "${points[@]}"; do
		[[ -z ${seen[$n]:-} && $n != 0 ]] || continue
		seen[$n]=1
		(( ++total <= ITERS )) || break 2
		fresh "$setup" || bail "$name: setup"
		pm=$(head -1 "$D/data/postmaster.pid")
		sql cm "SET sift.test_crash_dir = $n" "$op" >/dev/null 2>&1
		if ! await_exit "$pm"; then
			not_ok "$name, update $n of $k: the process was not killed"
			break 2
		fi
		if ! node_start cm; then
			not_ok "$name, update $n of $k: restart"
			diag "$(tail -20 "$D/postgres.log")"
			break 2
		fi
		if healthy; then
			sql cm "$op" >/dev/null && healthy && ok "$name, kill $([[ $n -gt 0 ]] && echo before || echo after) update ${n#-} of $k: recovery, answers, the operation again" ||
				{ not_ok "$name, kill at $n: after the operation run again"; diag "$(tail -5 "$D/postgres.log")"; }
		else
			not_ok "$name, kill at $n of $k: after recovery"
		fi
	done
	unset seen
done

# the worker that promotes a closed write buffer of a sift_table (sift.table_async_promote), killed in its first publication and after it
node_stop cm
node_conf cm "sift.async_maintenance = on
sift.table_async_promote = on
sift.maintenance_naptime = 200ms
sift.table_write_buffer_limit = 2048"
node_start cm || bail "restart for the worker"
TW="CREATE TABLE s (id bigint, v text) USING sift_table WITH (autovacuum_enabled = false); CREATE TABLE h (id bigint, v text)"
LOAD="DO \$\$ BEGIN FOR i IN 1 .. 30 LOOP
	INSERT INTO s SELECT g, repeat(chr(97 + (g % 26)::int), 300 + (g % 400)::int) FROM generate_series(i * 1000, i * 1000 + 999) g;
	INSERT INTO h SELECT g, repeat(chr(97 + (g % 26)::int), 300 + (g % 400)::int) FROM generate_series(i * 1000, i * 1000 + 999) g;
	COMMIT; END LOOP; END \$\$"
DIG="SELECT count(*) || ':' || sum(id) || ':' || sum(length(v)) FROM"
for n in 1 -1; do
	(( ++total <= ITERS )) || break
	sql cm "DROP TABLE IF EXISTS s, h" "$TW" >/dev/null
	printf 'sift.test_crash_worker = on\nsift.test_crash_dir = %d\n' "$n" >"$D/data/crash.conf"
	pg_ctl -D "$D/data" reload >/dev/null
	sleep 1
	pm=$(head -1 "$D/data/postmaster.pid")
	sql cm "$LOAD" >/dev/null 2>&1
	if ! await_exit "$pm"; then
		not_ok "the worker, update $n: the worker was not killed"
		rm -f "$D/data/crash.conf"
		break
	fi
	rm -f "$D/data/crash.conf"
	if ! node_start cm; then
		not_ok "the worker, update $n: restart"
		diag "$(tail -20 "$D/postgres.log")"
		break
	fi
	got=$(sql cm "$DIG s")
	check "the worker killed $([[ $n -gt 0 ]] && echo before || echo after) its first update: the rows are the twin's after recovery" test "$got" == "$(sql cm "$DIG h")"
	check "and the table, vacuumed, still holds them" test "$(sql cm "VACUUM s" "$DIG s")" == "$got"
done
echo "1..$NTEST"
finish
