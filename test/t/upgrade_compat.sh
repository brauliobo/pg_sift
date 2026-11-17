#!/usr/bin/env bash
# The library of a patch release over the files of the release before it (5.0.0 -> this tree; the update script of the extension is empty): one cluster writes the zoo of objects with the
# release (every index kind, the maintenance and recovery states a long-lived cluster holds, a quarantined index, the leftovers of a killed CONCURRENTLY build, a prepared transaction and a
# writer in flight), is stopped cleanly or crashed, and the same files are opened by this tree's library: every object verifies and answers as the heap, every write path and maintenance step
# works and converges, the leftovers are repaired or dropped, the zoo goes through pg_dump into a fresh cluster. Then the other way as a smoke (this tree's files read by the release; reported).
# SIFT_UPGRADE_OLD_REF names the release (default v5.0.0). One COMPAT line for each object kind, direction and shutdown, in the test output.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
OLD_REF=${SIFT_UPGRADE_OLD_REF:-v5.0.0}
ROWS=$(knob SIFT_COMPAT_ROWS 6000 3000)
newroot=$ROOT
build_old "$OLD_REF"; oldroot=$OLDROOT

. "$(dirname "$0")/lib_compat.bash"

# ---- the zoo, written by the release
node_init old 20 "$CONF"
use_root old "$oldroot"
node_start old || bail "the release does not start"
zoo_create old || bail "zoo setup failed"
sql old \
	"CREATE TABLE qt (id int, v int)" "INSERT INTO qt SELECT i, i % 7 FROM generate_series(1, $ROWS) i" "CREATE INDEX qt_idx ON qt USING sift_col (id, v)" \
	"CREATE TABLE iv (id int PRIMARY KEY, v int)" "INSERT INTO iv SELECT i, i % 7 FROM generate_series(1, 2000) i" \
	"VACUUM (ANALYZE) docs, kc, pt, events, fact, trk_c, trk_i, vs" "CHECKPOINT" >/dev/null || bail "zoo setup failed"
check "the release wrote one value store for the string columns" test "$(sql old "SELECT count(*) > 0 FROM pg_class WHERE relname LIKE '%_sift_values'")" == t

# the damage of a quarantined index: the release finds it, flags the metapage, and the flag is persistent
qfile=$(sql old "SELECT pg_relation_filepath('qt_idx')")
node_stop old
printf '\x02\x02\x02\x02\x02\x02\x02\x02\x02' | dd of="$(dir old)/data/$qfile" bs=1 seek=$((2 * 8192 + 8176)) conv=notrunc status=none
if LC_ALL=C pg_controldata "$(dir old)/data" | grep -q 'checksum version: *[1-9]'; then
	pg_checksums --disable -D "$(dir old)/data" >/dev/null && pg_checksums --enable -D "$(dir old)/data" >/dev/null || bail "pg_checksums failed"
fi
node_start old || bail "the release does not restart after the damage"
sql old "DELETE FROM qt WHERE id <= $((ROWS / 2))" "VACUUM qt" "SELECT count(*) FROM qt WHERE v = 3" >/dev/null 2>&1
check "the release quarantined the damaged index" test "$(sql old "SELECT index::text FROM sift.quarantine")" == qt_idx

# the leftovers of a CONCURRENTLY build killed while it waits for a writer
PGAPPNAME=zoo_blocker sql old "BEGIN" "LOCK TABLE iv IN ROW EXCLUSIVE MODE" "SELECT pg_sleep(600)" >/dev/null 2>&1 &
wait_for "test \$(sql old \"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'zoo_blocker' AND state = 'active'\") == 1" 30
PGAPPNAME=zoo_cic sql old "CREATE INDEX CONCURRENTLY iv_inv ON iv USING sift_col (id, v) WITH (track_changes = 'ctid')" >/dev/null 2>&1 &
wait_for "test \$(sql old \"SELECT count(*) FROM pg_index WHERE indexrelid = to_regclass('iv_inv')\") == 1" 60
sql old "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name IN ('zoo_cic', 'zoo_blocker')" >/dev/null
wait
check "the killed build left an INVALID index" test "$(sql old "SELECT NOT indisvalid FROM pg_index WHERE indexrelid = to_regclass('iv_inv')")" == t
TRIGGERS=$(sql old "SELECT string_agg(tgname, ',' ORDER BY tgname) FROM pg_trigger WHERE tgrelid = 'iv'::regclass AND NOT tgisinternal")
diag "triggers of the killed build's table: ${TRIGGERS:-none}"

# maintenance the release left pending: churn with no VACUUM and no worker (buffers unsealed, tombstones unfolded, scope pairs changed, rewrites due)
sql old "UPDATE docs SET n = n + 1 WHERE id % 3 = 0" "DELETE FROM docs WHERE id % 7 = 0" "INSERT INTO docs SELECT i, i % 9, i % 100, 'gamma' || i % 5 FROM generate_series($((ROWS + 1)), $((ROWS + 400))) i" \
	"UPDATE trk_c SET v = v + 1 WHERE id % 4 = 0" "DELETE FROM trk_c WHERE id % 11 = 0" "UPDATE trk_i SET v = v + 1 WHERE id % 4 = 0" "DELETE FROM trk_i WHERE id % 11 = 0" \
	"UPDATE kc SET v = v + 1 WHERE id % 5 = 0" "DELETE FROM tbl WHERE id % 13 = 0" "INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series($((ROWS + 1)), $((ROWS + 300))) i" \
	"DELETE FROM tenant_scope WHERE dim_a_id % 5 = 0" "INSERT INTO tenant_scope SELECT m, 1 + (m * 17) % 12 FROM generate_series(1, 400) m ON CONFLICT DO NOTHING" \
	"UPDATE events SET v = v + 1 WHERE id % 3 = 0" "UPDATE fact SET v = v + 1 WHERE id % 3 = 0" "DELETE FROM vs WHERE id % 9 = 0" "UPDATE vs SET n = n + 1 WHERE id % 4 = 0" >/dev/null || bail "churn failed"
DUE=$(sql old "SELECT count(*) FROM unnest(ARRAY[$COLIDX]) u WHERE sift.col_maintenance_due(u::regclass) IS NOT NULL")
diag "indexes with maintenance due in the release's cluster: $DUE"
check "the zoo has pending maintenance" test "$DUE" -gt 0

# writes in flight: a prepared transaction and a writer that never ends
sql old "BEGIN" "INSERT INTO docs SELECT i, i % 9, i % 100, 'prepared' FROM generate_series(900001, 900050) i" \
	"INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(900001, 900050) i" "UPDATE trk_c SET v = v + 100 WHERE id % 50 = 0" "PREPARE TRANSACTION 'zoo_prepared'" >/dev/null || bail "prepare failed"
sql old "BEGIN" "INSERT INTO docs SELECT i, 1, 1, 'inflight' FROM generate_series(950001, 950400) i" "DELETE FROM trk_i WHERE id % 3 = 0" "SELECT pg_sleep(600)" >/dev/null 2>&1 &
wait_for "test \$(sql old \"SELECT count(*) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()\") -ge 1" 30
HEAP_BEFORE=$(heap_counts old)

# the crash image (a power loss with the writer in flight), then the clean image of the same cluster after the release recovered it
crash old
mkdir -p "$D/snap"; cp -a "$(dir old)/data" "$D/snap/crash"; rm -f "$D/snap/crash/postmaster.pid"
wait
node_start old || bail "the release does not recover its own crash"
HEAP_REC=$(heap_counts old)
check "the release recovers its crash: the in-flight writer is gone, the prepared transaction stays" test "$(sql old "SELECT count(*) FROM pg_prepared_xacts")" == 1
node_stop old
cp -a "$(dir old)/data" "$D/snap/clean"; rm -f "$D/snap/clean/postmaster.pid"
diag "heap before the crash: $HEAP_BEFORE / after the release recovered: $HEAP_REC"

# ---- this tree's library over the files of the release
new_node() { # NAME OFFSET SNAPSHOT
	local d=$(dir "$1") conf
	rm -rf "$d"; mkdir -p "$d/sock"; echo "$2" >"$d/offset"; NODES+=("$1")
	cp -a "$3" "$d/data"
	conf=$d/data/postgresql.conf
	sed -i -E '/^(port|unix_socket_directories|extension_control_path|dynamic_library_path) = /d' "$conf"
	node_conf "$1" "port = $(port "$1")
unix_socket_directories = '$d/sock'"
	use_root "$1" "$newroot"
}

run_variant() { # VARIANT OFFSET
	local v=$1 n=nv_$1 off=$2 var
	new_node "$n" "$off" "$D/snap/$v"
	node_start "$n" || { not_ok "$v: this tree's library starts on the files of the release (recovery of the crash image included)"; diag "$(tail -20 "$(dir "$n")/postgres.log")"; return; }
	ok "$v: this tree's library starts on the files of the release"
	check "$v: nothing in the log of the start is a PANIC or a sift error" test "$(grep -cE 'PANIC|sift index .* is corrupted|TRAP' "$(dir "$n")/postgres.log")" == 0
	check "$v: the prepared transaction is still there" test "$(sql "$n" "SELECT count(*) FROM pg_prepared_xacts")" == 1
	check "$v: the quarantine list is the one the release wrote" test "$(sql "$n" "SELECT index::text FROM sift.quarantine")" == qt_idx
	check "$v: the quarantined index is planned on the heap and its rows are the heap's" test "$(sql "$n" "SELECT count(*) FROM qt WHERE v = 3")" == "$(sql "$n" "SET enable_indexscan = off" "SET sift.enable_scan = off" "SET sift.enable_col_scan = off" "SELECT count(*) FROM qt WHERE v = 3")"
	check "$v: the extension is at 5.0 and the library says which build runs" test "$(sql "$n" "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 5.0
	diag "$v: sift.version() = $(sql "$n" "SELECT sift.version()")"
	check "$v: sift.check() has no finding beyond the damage and the leftover" sound_all "$n" "qt_idx,iv_inv"
	compat old-files-new-lib "$v" "$n"
	out=$(sql "$n" "SELECT kind || ':' || object || ':' || coalesce(command, '') FROM sift.upgrade_commands() ORDER BY object::text" 2>&1)
	check "$v: sift.upgrade_commands() answers" test "$(grep -c ERROR <<<"$out")" == 0
	diag "$v: upgrade_commands: ${out:-empty}"
	check "$v: ALTER EXTENSION UPDATE is a no-op that works" sql "$n" "ALTER EXTENSION pg_sift UPDATE"
	check "$v: the extension version after the update" test "$(sql "$n" "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")" == 5.0

	# the prepared transaction ends the way the variant says
	if [[ $v == clean ]]; then sql "$n" "COMMIT PREPARED 'zoo_prepared'" >/dev/null; else sql "$n" "ROLLBACK PREPARED 'zoo_prepared'" >/dev/null; fi
	check "$v: the prepared transaction ends" test "$(sql "$n" "SELECT count(*) FROM pg_prepared_xacts")" == 0
	compat prepared-ended "$v" "$n"

	# every write path
	check "$v: inserts, updates and deletes on every kind" sql "$n" \
		"INSERT INTO docs SELECT i, i % 9, i % 100, 'delta' || i % 3 FROM generate_series(800001, 800300) i" "UPDATE docs SET n = n + 2 WHERE id % 5 = 0" "DELETE FROM docs WHERE id % 17 = 0" \
		"INSERT INTO kc SELECT i, i % 20, i % 50 FROM generate_series(800001, 800300) i" "UPDATE kc SET v = v + 1 WHERE id % 6 = 0" \
		"INSERT INTO pt SELECT i, i % 9, i % 100 FROM generate_series(800001, 800300) i" "DELETE FROM pt WHERE id % 19 = 0" \
		"INSERT INTO events SELECT i, 1 + i % 420, now(), i % 10 FROM generate_series(800001, 800200) i" "UPDATE tenant_scope SET tenant_id = tenant_id % 12 + 1 WHERE dim_a_id % 7 = 0" \
		"INSERT INTO fact SELECT i, 1 + i % 50, i % 30 FROM generate_series(800001, 800200) i" "INSERT INTO dim SELECT i, 'd' || i FROM generate_series(51, 60) i" \
		"INSERT INTO trk_c SELECT i, i % 8, i % 60 FROM generate_series(800001, 800200) i" "UPDATE trk_c SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM trk_c WHERE id % 23 = 0" \
		"INSERT INTO trk_i SELECT i, i % 8, i % 60 FROM generate_series(800001, 800200) i" "UPDATE trk_i SET v = v + 1 WHERE id % 7 = 0" "DELETE FROM trk_i WHERE id % 23 = 0" \
		"INSERT INTO tbl SELECT i, i % 9, i % 100 FROM generate_series(800001, 800300) i" "UPDATE tbl SET n = n + 1 WHERE id % 5 = 0" "DELETE FROM tbl WHERE id % 29 = 0" \
		"INSERT INTO vs SELECT i, md5(i::text) || md5('x' || i), 'b' || i % 13, i % 10 FROM generate_series(800001, 800200) i" "DELETE FROM vs WHERE id % 31 = 0"
	compat after-writes "$v" "$n"
	check "$v: VACUUM of every table" sql "$n" "VACUUM (ANALYZE) docs, kc, pt, events, fact, dim, trk_c, trk_i, vs"
	check "$v: sift_table maintenance and VACUUM FULL (the rewrite)" sql "$n" "SELECT * FROM sift.table_maintain('tbl')" "VACUUM FULL tbl"
	check "$v: merge of every sift_col index, forced" sql "$n" "SELECT sift.merge(u::regclass, true) FROM unnest(ARRAY[$COLIDX, 'docs_idx']) u"
	check "$v: col_maintain of every sift_col index" sql "$n" "SELECT sift.col_maintain(u::regclass) FROM unnest(ARRAY[$COLIDX]) u"
	compat after-maintenance "$v" "$n"

	# the maintenance workers converge the churn the release left and the writes after it
	sql "$n" "UPDATE docs SET n = n + 1 WHERE id % 2 = 0" "DELETE FROM trk_c WHERE id % 3 = 0" "DELETE FROM trk_i WHERE id % 3 = 0" "UPDATE kc SET v = v + 1 WHERE id % 2 = 0" >/dev/null
	sql "$n" "ALTER SYSTEM SET sift.async_maintenance = on" "ALTER SYSTEM SET sift.col_auto_merge = on" "ALTER SYSTEM SET sift.col_auto_merge_wal_pct = 10" "ALTER SYSTEM SET autovacuum = on" "ALTER SYSTEM SET autovacuum_naptime = 2" "SELECT pg_reload_conf()" >/dev/null
	due() { sql "$n" "SELECT count(*) FROM unnest(ARRAY[$COLIDX]) u WHERE sift.col_maintenance_due(u::regclass) IS NOT NULL"; }
	wait_for "test \$(due) == 0" 60
	diag "$v: indexes still due after 60 s of the workers (their WAL pacing may hold them): $(due)"
	for _ in 1 2 3; do sql "$n" "SELECT sift.col_maintain(u::regclass) FROM unnest(ARRAY[$COLIDX]) u WHERE sift.col_maintenance_due(u::regclass) IS NOT NULL" >/dev/null; done
	check "$v: maintenance converges: col_maintain leaves no index due" test "$(due)" == 0
	check "$v: the workers left no error in the log" test "$(grep -cE 'ERROR:  .*sift|PANIC|TRAP' "$(dir "$n")/postgres.log")" == 0
	sql "$n" "ALTER SYSTEM RESET autovacuum" "ALTER SYSTEM RESET sift.async_maintenance" "ALTER SYSTEM RESET sift.col_auto_merge" "ALTER SYSTEM RESET sift.col_auto_merge_wal_pct" "SELECT pg_reload_conf()" >/dev/null
	compat after-workers "$v" "$n"

	# the leftovers
	if [[ $v == clean ]]; then
		check "$v: REINDEX of the INVALID leftover makes it valid and it answers" sql "$n" "REINDEX INDEX iv_inv"
		check "$v: ... valid" test "$(sql "$n" "SELECT indisvalid FROM pg_index WHERE indexrelid = 'iv_inv'::regclass")" == t
		check "$v: ... sound" sound "$n" "'iv_inv'"
	else
		check "$v: DROP INDEX of the INVALID leftover" sql "$n" "DROP INDEX iv_inv"
		check "$v: ... and no trigger of the killed build is left on its table" test "$(sql "$n" "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'iv'::regclass AND NOT tgisinternal")" == 0
		check "$v: ... and a new build works" sql "$n" "CREATE INDEX iv_inv ON iv USING sift_col (id, v) WITH (track_changes = 'ctid')"
	fi
	diag "$v: triggers on the table of the killed build after the repair: $(sql "$n" "SELECT coalesce(string_agg(tgname, ','), 'none') FROM pg_trigger WHERE tgrelid = 'iv'::regclass AND NOT tgisinternal")"
	check "$v: REINDEX puts the quarantined index back" sql "$n" "REINDEX INDEX qt_idx"
	check "$v: ... the list is empty" test "$(sql "$n" "SELECT count(*) FROM sift.quarantine")" == 0
	check "$v: ... and it answers as the heap" agree "$n" qt_idx "SELECT v, count(*) FROM qt GROUP BY v ORDER BY v"
	check "$v: sift.check() is empty for the whole database" sound_all "$n"
	check "$v: the second update and the preflight report" sql "$n" "ALTER EXTENSION pg_sift UPDATE" "SELECT count(*) FROM sift.upgrade_check()"
	[[ $v != clean ]] || dump_restore "$n"
	node_stop "$n"
	check "$v: clean shutdown after the whole run" test "$(grep -c 'database system is shut down' "$(dir "$n")/postgres.log")" -ge 1
}

# the zoo through pg_dump into a fresh cluster of this tree
dump_restore() { # NODE
	local n=$1 r=nv_restore d
	d=$(dir "$n")
	node_init "$r" 24 "$CONF" ; use_root "$r" "$newroot"
	node_start "$r" || bail "the fresh cluster does not start"
	pg_dump -h "$d/sock" -p "$(port "$n")" -U postgres -Fc -f "$d/zoo.dump" postgres
	pg_dump -h "$d/sock" -p "$(port "$n")" -U postgres -Fp -f "$d/zoo.sql" postgres
	sql "$r" "CREATE DATABASE zoo2" >/dev/null
	out=$(pg_restore -h "$(dir "$r")/sock" -p "$(port "$r")" -U postgres -d zoo2 --no-owner "$d/zoo.dump" 2>&1)
	check "pg_restore of the zoo into a fresh cluster has no error" test "$(grep -c 'error' <<<"$out")" == 0
	z() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$r")/sock" -p "$(port "$r")" -U postgres -d zoo2 "${@/#/--command=}"; }
	local o kind ix q p ok
	for o in "${OBJECTS[@]}"; do
		IFS='|' read -r kind ix q p <<<"$o"
		ok=FAIL
		[[ $(z "SELECT c.index::text || c.problem FROM unnest(coalesce((SELECT array_agg(inhrelid::regclass) FROM pg_inherits WHERE inhparent = '$ix'::regclass), ARRAY['$ix'::regclass])) u, LATERAL sift.check(u) c" 2>&1) == "" ]] && [[ $(z "${ON[@]}" "$q" 2>&1 | tr '\n' ' ') == "$(sql "$n" "${ON[@]}" "$q" 2>&1 | tr '\n' ' ')" ]] && ok=PASS
		echo "# COMPAT $kind dump-restore clean $ok"
		check "restore: $kind ($ix) verifies and answers as the source" test "$ok" == PASS
	done
	check "restore: sift.check() of the restored database is empty" test "$(z "SELECT count(*) FROM sift.check()")" == 0
	check "restore: plain SQL dump restores too" test "$(sql "$r" "CREATE DATABASE zoo3" >/dev/null; psql -X -At -q -h "$(dir "$r")/sock" -p "$(port "$r")" -U postgres -d zoo3 -f "$d/zoo.sql" 2>&1 | grep -c ERROR)" == 0
	node_stop "$r"
}

# the other way, a smoke: the files this tree's library wrote, opened by the release (reported; downgrade is not a gate)
reverse_smoke() { # VARIANT
	local v=$1 n=nv_$1 o kind ix q p res
	use_root "$n" "$oldroot"
	if ! node_start "$n"; then echo "# COMPAT all new-files-old-lib $v FAIL (does not start)"; diag "$(tail -5 "$(dir "$n")/postgres.log")"; ok "$v reverse smoke: the release did not start on the files of this tree (reported)"; return; fi
	for o in "${OBJECTS[@]}"; do
		IFS='|' read -r kind ix q p <<<"$o"
		res=FAIL; { sound "$n" "'$ix'" && agree "$n" "$ix" "$q" "$p"; } && res=PASS
		echo "# COMPAT $kind new-files-old-lib $v $res"
		[[ $res == PASS ]] || diag "reverse $kind ($ix): $(head -c 300 <<<"$out")"
	done
	ok "$v reverse smoke done (reported, not a gate)"
	node_stop "$n"
}

run_variant clean 30
run_variant crash 34
reverse_smoke clean
reverse_smoke crash
plan "$NTEST"
finish
