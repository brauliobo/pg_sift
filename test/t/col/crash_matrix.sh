#!/usr/bin/env bash
# The crash matrix of the operations that publish a new directory of an index: build, write-buffer promotion, merge and VACUUM of a BM25 index and of a
# sift_col index (with the options cube_minmax, key_columns and hll_columns, column format minors 7, 15 and 20), the sealing of a sift_table and the promotion of its write buffer by the
# worker. Every publication (store_update) is a named point of the testing build: sift.test_crash_dir = n kills the process with SIGKILL before the
# nth directory update (n > 0) or after it (n < 0), its log flushed, so that the segment is written and the directory still the old one, or the
# directory is the new one and nothing after it ran. For each operation the points are the first and the last of its updates (more in the full
# run). After the restart (wal_consistency_checking = generic: crash recovery compares every page it rebuilds with the page the primary wrote) the
# indexes verify (heapallindexed), the store checks pass, the answers through the indexes equal the plain plan and what the table held before, and
# the operation, run again, completes and leaves it the same. An index with a scope (the scope_* scenarios) also has the scope entries of every row of its entity segments after the restart: the
# promotion, the merge and the refill of its scope segments are each one switch of the directory. SIFT_CRASHMX_ITERS caps the number of crashes (default 70, 20 quick: the first and the last update of every operation).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_CRASHMX_ITERS 70 20)
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

. "$(dirname "$0")/crash_scenarios.bash"

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

# the worker that promotes a closed write buffer of a sift_table (sift.async_maintenance), killed in its first publication and after it
node_stop cm
node_conf cm "sift.async_maintenance = on
sift.maintenance_naptime = 200ms"
node_start cm || bail "restart for the worker"
TW="CREATE TABLE s (id bigint, v text) USING sift_table WITH (autovacuum_enabled = false); SELECT sift.table_set_option('s', 'write_buffer_limit', '2048kB'); SELECT sift.table_set_option('s', 'async_promote', 'on'); CREATE TABLE h (id bigint, v text)"
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
	wait_for "setting_is cm sift.test_crash_dir $n" 30
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
done_testing
finish
