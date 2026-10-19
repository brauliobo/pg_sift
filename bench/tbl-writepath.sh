#!/usr/bin/env bash
# One transaction that writes a lot of rows through single-row INSERTs into a sift_table with a primary key, the way an application loop or a
# migration script does. Run once under the perf lock (it is a load, not a gate: the regress test tbl_writepath and the isolation and TAP tests
# lower the directory limit to cover the same paths in seconds). It builds and starts its own cluster under ~/.cache, inside the caps of the
# other benchmarks (2 CPUs, 12 GB, no swap), and prints a line "RESULT key=value ..." per measurement, so that two builds compare with diff:
# run it from the worktree of each build.
#
#   bench/tbl-writepath.sh                 all modes, 10000000 rows in the main one
#   ROWS=100000 bench/tbl-writepath.sh     the smoke test
#   MODES=loop ROWS=10000000 bench/tbl-writepath.sh
#
#   loop        DO $$ BEGIN FOR i IN 1..ROWS LOOP INSERT ...; END LOOP; END $$ then COMMIT: the time, the peak memory of the backend (VmHWM),
#               the most directory entries a second session sees, the WAL written, the size of the relation; count, sums and an index probe
#               through a plain scan and through the aggregate plan before and after VACUUM (the pre-aggregates apply only after the freeze);
#               then the same load with ROLLBACK and VACUUM: the space comes back (ROLLBACK_RUN=0 skips this half)
#   psql        PSQL_ROWS (ROWS / 10) INSERT statements through psql, one round trip each, in one transaction
#   savepoint   SP_ROWS (ROWS / 10) iterations of BEGIN ... EXCEPTION ... END, a subtransaction (and its xid) per row
#   throughput  TP_ROWS (ROWS / 20) single-row INSERTs per table access method (AMS, default "sift_table heap"): one transaction each
#               (autocommit), all in one transaction from psql, and in one transaction from a PL/pgSQL loop; rows per second
#
# Environment: ROWS PSQL_ROWS SP_ROWS TP_ROWS MODES (comma list) AMS ROLLBACK_RUN, SIFT_TMP (default ~/.cache/sift-bench-writepath) and SIFT_PORT
# (56100-56299, default a free one in 56150-56250), LOW_LIMITS=1 (the directory of 16 entries and write buffers of 2 MB in every session, a testing
# setting, to see the pressure with a few hundred thousand rows), KEEP=1 (do not remove the cluster), LOCK_WAIT (seconds to wait for the perf
# lock, default 900; it never queues longer), NO_CAPS=1 (no systemd scope).
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=${ROWS:-10000000}
PSQL_ROWS=${PSQL_ROWS:-$((ROWS / 10))}
SP_ROWS=${SP_ROWS:-$((ROWS / 10))}
TP_ROWS=${TP_ROWS:-$((ROWS / 20))}
MODES=${MODES:-loop,psql,savepoint,throughput}
AMS=${AMS:-sift_table heap}
ROLLBACK_RUN=${ROLLBACK_RUN:-1}
LOCK_WAIT=${LOCK_WAIT:-900}

# the whole run in 2 cores and 12 GB, whatever the caller did
if [[ -z ${WP_SCOPED:-} && -z ${NO_CAPS:-} ]] && command -v systemd-run >/dev/null; then
	exec systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 -p OOMPolicy=continue -p CPUQuota=200% env WP_SCOPED=1 "$0" "$@"
fi

# the perf lock: poll for it, never queue
exec 9>>/tmp/sift-perf.lock
for ((waited = 0; ; waited += 10)); do
	flock -n 9 && break
	((waited < LOCK_WAIT)) || { echo "tbl-writepath: /tmp/sift-perf.lock is held after ${LOCK_WAIT}s, try later" >&2; exit 2; }
	sleep 10
done

export SIFT_TMP=${SIFT_TMP:-$HOME/.cache/sift-bench-writepath}
if [[ -z ${SIFT_PORT:-} ]]; then
	for _ in $(seq 1 50); do
		SIFT_PORT=$((56150 + RANDOM % 101))
		ss -ltn 2>/dev/null | grep -q ":$SIFT_PORT " || break
	done
fi
export SIFT_PORT
PG="$REPO/scripts/pgtest.sh"
q() { "$PG" psql -X -qAt "$@"; }
now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.2f", b - a }'; }
rate() { awk -v n="$1" -v s="$2" 'BEGIN { printf "%d", (s > 0) ? n / s : 0 }'; }
result() { echo "RESULT $*"; }
fail=0
check() { if [[ $2 == "$3" ]]; then echo "ok   $1: $2"; else echo "FAIL $1: got $2, want $3"; fail=1; fi; }
[[ -z ${LOW_LIMITS:-} ]] || export PGOPTIONS="-c sift.table_max_segments=16 -c sift.table_write_buffer_limit=2048"

WORK=$(mktemp -d "${TMPDIR:-$HOME/.cache}/sift-wp-work.XXXXXX")
cleanup() {
	touch "$WORK/stop.mem" "$WORK/stop.dir"
	wait 2>/dev/null
	[[ -n ${KEEP:-} ]] || { "$PG" destroy >/dev/null 2>&1; }
	rm -rf "$WORK"
}
trap cleanup EXIT

# a cluster of its own: the default WAL segments, room for the WAL of a big load, no autovacuum (pgtest.sh start); the server does not hold the lock
cluster_up() {
	[[ -f $SIFT_TMP/root/.flavor ]] || "$PG" install >/dev/null || exit 1
	if [[ ! -f $SIFT_TMP/data/PG_VERSION ]]; then
		mkdir -p "$SIFT_TMP/sock"
		"$PG" new-data "$SIFT_TMP/data" wal || exit 1
		cat >>"$SIFT_TMP/data/postgresql.conf" <<-EOF
			port = $SIFT_PORT
			listen_addresses = ''
			unix_socket_directories = '$SIFT_TMP/sock'
			$("$PG" conf)
			fsync = off
			synchronous_commit = off
			shared_buffers = 1GB
			max_wal_size = 16GB
			checkpoint_timeout = 30min
			max_worker_processes = 16
			autovacuum = off
		EOF
	fi
	"$PG" start 9>&- >/dev/null || exit 1
	q -c "CREATE EXTENSION IF NOT EXISTS pg_sift" >/dev/null
	q -c "CREATE EXTENSION IF NOT EXISTS pg_sift_test" >/dev/null 2>&1 || true   # the testing build only
}
cluster_up
echo "tbl-writepath: $(q -c "SELECT sift.version()") on port $SIFT_PORT, $(q -c 'SHOW shared_buffers') shared buffers, build $(git -C "$REPO" rev-parse --short HEAD)"

# ---------- the rows ----------
# the INSERT of row i inside PL/pgSQL, and as a literal statement (awk), with the same values
ROW_PLSQL="INSERT INTO @T VALUES (i, i % 100000, (ARRAY['BR','US','DE','FR','JP','IN'])[1 + i % 6], (ARRAY['new','paid','shipped','returned'])[1 + i % 4],
	(i % 99999) / 100.0, 1 + i % 9, timestamp '2026-01-01' + (i % 28) * interval '1 day' + (i % 24) * interval '1 hour', 'order ' || i)"
row_awk() { # TABLE N
	awk -v t="$1" -v n="$2" 'BEGIN { split("BR US DE FR JP IN", c, " "); split("new paid shipped returned", s, " ")
		for (i = 1; i <= n; i++)
			printf "INSERT INTO %s VALUES (%d, %d, '"'"'%s'"'"', '"'"'%s'"'"', %.2f, %d, '"'"'2026-01-%02d %02d:00:00'"'"', '"'"'order %d'"'"');\n", t, i, i % 100000, c[1 + i % 6], s[1 + i % 4], (i % 99999) / 100, 1 + i % 9, 1 + i % 28, i % 24, i }'
}
create_orders() { # TABLE [ACCESS METHOD]
	q -c "SET client_min_messages = warning" -c "DROP TABLE IF EXISTS $1" -c "CREATE TABLE $1 (id bigint PRIMARY KEY, customer_id int NOT NULL, country text NOT NULL, status text NOT NULL,
		amount numeric(10,2) NOT NULL, quantity smallint NOT NULL, created_at timestamp NOT NULL, title text NOT NULL) USING ${2:-sift_table} WITH (autovacuum_enabled = false)" >/dev/null
}
plsql_loop() { # TABLE N: a DO block of single-row INSERTs
	echo "DO \$\$ BEGIN FOR i IN 1..$2 LOOP ${ROW_PLSQL//@T/$1}; END LOOP; END \$\$;"
}
subxact_loop() { # TABLE N
	echo "DO \$\$ BEGIN FOR i IN 1..$2 LOOP BEGIN ${ROW_PLSQL//@T/$1}; EXCEPTION WHEN unique_violation THEN NULL; END; END LOOP; END \$\$;"
}

# ---------- sampling ----------
# the peak (VmHWM) of the backend with the pid in FILE, until STOP exists; writes KB to OUT
watch_memory() {
	local pidfile=$1 out=$2 pid hwm peak=0
	while [[ ! -e $WORK/stop.mem ]]; do
		pid=$(head -1 "$pidfile" 2>/dev/null)
		if [[ $pid =~ ^[0-9]+$ ]]; then
			hwm=$(awk '/^VmHWM/ {print $2}' "/proc/$pid/status" 2>/dev/null)
			[[ -n ${hwm:-} ]] && ((hwm > peak)) && peak=$hwm
		fi
		echo "$peak" >"$out"
		sleep 0.2
	done
}
# the most entries the directory of TABLE had, seen from a second session once a second, until STOP exists; writes it to OUT
watch_directory() {
	local table=$1 out=$2 n max=0
	while [[ ! -e $WORK/stop.dir ]]; do
		n=$(q -c "SELECT count(*) FROM sift.table_segments('$table')" 2>/dev/null)
		[[ $n =~ ^[0-9]+$ ]] && ((n > max)) && max=$n
		echo "$max" >"$out"
		sleep 1
	done
}
watchers_start() { # TABLE
	rm -f "$WORK"/stop.mem "$WORK"/stop.dir "$WORK/pid" "$WORK/mem" "$WORK/dir"
	echo 0 >"$WORK/mem"; echo 0 >"$WORK/dir"
	watch_memory "$WORK/pid" "$WORK/mem" 9>&- &
	MEMPID=$!
	watch_directory "$1" "$WORK/dir" 9>&- &
	DIRPID=$!
}
watchers_stop() {
	touch "$WORK/stop.mem" "$WORK/stop.dir"
	wait "$MEMPID" "$DIRPID" 2>/dev/null
}
wal_lsn() { q -c "SELECT pg_current_wal_lsn()"; }
wal_bytes() { q -c "SELECT pg_wal_lsn_diff('$(wal_lsn)', '$1')"; }

# ---------- the checks ----------
SUMS='SELECT count(*) || '"':'"' || sum(id) FROM @T'
AGG='SELECT country || status || '"':'"' || count(*) || '"':'"' || sum(amount) || '"':'"' || sum(quantity) FROM @T GROUP BY country, status ORDER BY 1'
verify() { # LABEL TABLE N
	local t=$2 n=$3 plain
	check "$1: count and sum" "$(q -c "${SUMS//@T/$t}")" "$n:$((n * (n + 1) / 2))"
	check "$1: index probe" "$(q -c "SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM $t WHERE id BETWEEN $((n / 2)) AND $((n / 2 + 999))")" 1000
	plain=$(q -c "SET sift.enable_agg = off; ${AGG//@T/$t}" | md5sum)
	check "$1: aggregate scan = plain scan" "$(q -c "SET sift.enable_agg = on; ${AGG//@T/$t}" | md5sum)" "$plain"
	check "$1: no row is wrong" "$(q -c "SELECT count(*) FROM $t WHERE title <> 'order ' || id OR customer_id <> id % 100000")" 0
}
space() { q -c "SELECT 'relation_blocks=' || relation_blocks || ' high_water=' || high_water || ' free_blocks=' || free_blocks FROM sift.table_space('$1')"; }
blocks() { q -c "SELECT relation_blocks FROM sift.table_space('$1')"; }
vacuum_timed() { local t0; t0=$(now); q -c "VACUUM $1" >/dev/null; echo "$(since "$t0")"; }
# VACUUM twice with a transaction between: what the first releases is reusable once another transaction has passed
vacuum_space_back() { q -c "VACUUM $1" -c "SELECT pg_current_xact_id() IS NOT NULL" -c "VACUUM $1" >/dev/null; }

# ---------- the main load: one transaction, ROWS single-row INSERTs ----------
# load_run LABEL TABLE N SQL-LOADER END (COMMIT or ROLLBACK): the load as one transaction in one psql session
load_run() {
	local label=$1 table=$2 n=$3 loader=$4 end=$5 lsn t0 t1 t2 t3 pid
	lsn=$(wal_lsn)
	cat >"$WORK/load.sql" <<-EOF
		BEGIN;
		SELECT 'pid|' || pg_backend_pid();
		SELECT 't0|' || extract(epoch FROM clock_timestamp());
		$($loader "$table" "$n")
		SELECT 't1|' || extract(epoch FROM clock_timestamp());
		SELECT 'seen|' || count(*) FROM $table;
		SELECT 't2|' || extract(epoch FROM clock_timestamp());
		$end;
		SELECT 't3|' || extract(epoch FROM clock_timestamp());
	EOF
	watchers_start "$table"
	"$PG" psql -X -qAt -v ON_ERROR_STOP=1 -f "$WORK/load.sql" >"$WORK/out" 2>&1 9>&- &
	local psql=$!
	while kill -0 $psql 2>/dev/null; do
		[[ -s $WORK/pid ]] || { sed -n 's/^pid|//p' "$WORK/out" >"$WORK/pid"; }
		sleep 0.2
	done
	local ok=0
	wait $psql || { echo "FAIL $label: the session failed"; tail -5 "$WORK/out"; fail=1; ok=1; }
	watchers_stop
	((ok == 0)) || return 1
	t0=$(sed -n 's/^t0|//p' "$WORK/out"); t1=$(sed -n 's/^t1|//p' "$WORK/out"); t3=$(sed -n 's/^t3|//p' "$WORK/out"); t2=$(sed -n 's/^t2|//p' "$WORK/out")
	[[ -n $t0 && -n $t1 && -n $t3 ]] || { echo "FAIL $label: no timings"; fail=1; return 1; }
	local secs commit
	secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }')
	commit=$(awk -v a="$t2" -v b="$t3" 'BEGIN { printf "%.2f", b - a }')
	echo "$label: $n rows in $secs s ($(rate "$n" "$secs") rows/s), $end $commit s, backend peak memory $(($(cat "$WORK/mem") / 1024)) MB, directory entries at most $(cat "$WORK/dir"), WAL $(($(wal_bytes "$lsn") / 1048576)) MB, $(space "$table")"
	result "mode=$label rows=$n seconds=$secs rows_per_s=$(rate "$n" "$secs") end_seconds=$commit peak_mb=$(($(cat "$WORK/mem") / 1024)) max_entries=$(cat "$WORK/dir") wal_mb=$(($(wal_bytes "$lsn") / 1048576))"
	check "$label: count seen by the writer" "$(sed -n 's/^seen|//p' "$WORK/out")" "$n"
}

run_loop() {
	create_orders orders
	load_run "loop-commit" orders "$ROWS" plsql_loop COMMIT || return 0
	local t
	verify "loop, committed" orders "$ROWS"
	t=$(vacuum_timed orders)
	echo "VACUUM: $t s, $(space orders)"
	result "mode=loop-vacuum seconds=$t"
	verify "loop, vacuumed" orders "$ROWS"
	check "loop: no group carries a writer after VACUUM" "$(q -c "SELECT sift._test_tbl_tagged('orders')" 2>/dev/null || echo 0)" 0
	q -c "DROP TABLE orders" >/dev/null
	[[ $ROLLBACK_RUN == 1 ]] || return 0
	create_orders orders_rb
	load_run "loop-rollback" orders_rb "$ROWS" plsql_loop ROLLBACK || return 0
	local before after
	before=$(blocks orders_rb)
	check "loop, rolled back: no rows" "$(q -c "SELECT count(*) FROM orders_rb")" 0
	vacuum_space_back orders_rb
	after=$(blocks orders_rb)
	echo "loop, rolled back: relation $before blocks, $after blocks after VACUUM"
	result "mode=loop-rollback-space blocks_before=$before blocks_after=$after"
	check "loop, rolled back: the space comes back" "$((after < before / 4 || before < 1000))" 1
	q -c "DROP TABLE orders_rb" >/dev/null
}

run_psql() {
	create_orders orders_psql
	local lsn t0 secs peak=0 hwm pid
	lsn=$(wal_lsn)
	watchers_start orders_psql
	t0=$(now)
	row_awk orders_psql "$PSQL_ROWS" | PGAPPNAME=wp-psql "$PG" psql -X -q -1 -v ON_ERROR_STOP=1 -f - >"$WORK/out" 2>&1 9>&- &
	local psql=$!
	while kill -0 $psql 2>/dev/null; do
		[[ -s $WORK/pid ]] || q -c "SELECT pid FROM pg_stat_activity WHERE application_name = 'wp-psql'" >"$WORK/pid"
		sleep 0.5
	done
	wait $psql || { echo "FAIL psql: the session failed"; tail -5 "$WORK/out"; fail=1; }
	watchers_stop
	secs=$(since "$t0")
	echo "psql: $PSQL_ROWS statements, one round trip each, in $secs s ($(rate "$PSQL_ROWS" "$secs") rows/s), backend peak memory $(($(cat "$WORK/mem") / 1024)) MB, directory entries at most $(cat "$WORK/dir"), WAL $(($(wal_bytes "$lsn") / 1048576)) MB, $(space orders_psql)"
	result "mode=psql rows=$PSQL_ROWS seconds=$secs rows_per_s=$(rate "$PSQL_ROWS" "$secs") peak_mb=$(($(cat "$WORK/mem") / 1024)) max_entries=$(cat "$WORK/dir")"
	verify "psql, committed" orders_psql "$PSQL_ROWS"
	vacuum_timed orders_psql >/dev/null
	verify "psql, vacuumed" orders_psql "$PSQL_ROWS"
	q -c "DROP TABLE orders_psql" >/dev/null
}

run_savepoint() {
	create_orders orders_sp
	load_run "savepoint-commit" orders_sp "$SP_ROWS" subxact_loop COMMIT || return 0
	verify "savepoint, committed" orders_sp "$SP_ROWS"
	vacuum_timed orders_sp >/dev/null
	verify "savepoint, vacuumed" orders_sp "$SP_ROWS"
	q -c "DROP TABLE orders_sp" >/dev/null
}

# rows per second of single-row INSERTs: a transaction each (autocommit), all in one transaction from psql, and from a PL/pgSQL loop
run_throughput() {
	local am t0 secs n=$TP_ROWS
	for am in $AMS; do
		create_orders tp "$am"
		t0=$(now)
		row_awk tp "$n" | "$PG" psql -X -q -v ON_ERROR_STOP=1 -f - >/dev/null 9>&-
		secs=$(since "$t0")
		echo "throughput $am: $n INSERTs, a transaction each, $secs s ($(rate "$n" "$secs") rows/s)"
		result "mode=throughput-autocommit am=$am rows=$n seconds=$secs rows_per_s=$(rate "$n" "$secs")"
		create_orders tp "$am"
		t0=$(now)
		row_awk tp "$n" | "$PG" psql -X -q -1 -v ON_ERROR_STOP=1 -f - >/dev/null 9>&-
		secs=$(since "$t0")
		echo "throughput $am: $n INSERTs in one transaction from psql, $secs s ($(rate "$n" "$secs") rows/s)"
		result "mode=throughput-one-transaction am=$am rows=$n seconds=$secs rows_per_s=$(rate "$n" "$secs")"
		create_orders tp "$am"
		t0=$(now)
		q -c "BEGIN" -c "$(plsql_loop tp "$n")" -c "COMMIT" >/dev/null 9>&-
		secs=$(since "$t0")
		echo "throughput $am: $n INSERTs in one transaction from a PL/pgSQL loop, $secs s ($(rate "$n" "$secs") rows/s)"
		result "mode=throughput-plpgsql-loop am=$am rows=$n seconds=$secs rows_per_s=$(rate "$n" "$secs")"
		q -c "DROP TABLE tp" >/dev/null
	done
}

for mode in ${MODES//,/ }; do
	case $mode in
		loop) run_loop ;;
		psql) run_psql ;;
		savepoint) run_savepoint ;;
		throughput) run_throughput ;;
		*) echo "tbl-writepath: unknown mode $mode (loop, psql, savepoint, throughput)" >&2; exit 2 ;;
	esac
done
((fail)) && { echo "tbl-writepath: FAILED"; exit 1; }
echo "tbl-writepath: ok"
