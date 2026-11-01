#!/usr/bin/env bash
# The online shrink: while a physical standby streams, VACUUM moves the segment parts that lie above the end the file can have into the reusable holes below it
# (no exclusive lock; the runs left behind are held by the horizon like those of a rewrite) and the online cut takes the free end. A merge leaves the file twice the
# live bytes with the last block live; the file is back near the live bytes in a few VACUUMs: with a standby query holding the table (the blocks it can read are not
# reused, the file does not grow), with inserts, deletes, merges and scans running, and after a kill when the pages are copied and when the move is published.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_SH_ROWS 300000 150000)
plan 29
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 512MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s
max_standby_streaming_delay = -1"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
D=$(dir primary)
size() { sql primary "SELECT pg_relation_size('t_idx')"; }
live() { sql primary "SELECT bytes FROM sift.col_index_info('t_idx')"; }
tight() {
	test "$(size)" -le "$(( $(live) * ${1:-120} / 100 ))" && return 0
	echo "# file $(size) live $(live) tail: $(sql primary "SELECT tail_what, tail_gen, tail_end, reusable_blocks, pending_blocks, shrink_to, shrink_blocks FROM sift.col_space('t_idx')")" >&2
	return 1
}
loose() { test "$(size)" -gt "$(( $(live) * ${1:-16} / 10 ))"; }
vacuum() { sql primary "SET sift.async_maintenance = off" "VACUUM t" >/dev/null; }
# the standby reports its horizon every second (hot_standby_feedback): a pass moves what the horizon has passed
past() { sql primary "SELECT pg_current_xact_id()" >/dev/null; catchup primary standby; sleep 1.2; }
# VACUUM until the file is within PERCENT of the live bytes: a pass moves a share of the file, and the cut follows once the horizon has passed what it moved
vacuum_until() {
	for _ in $(seq 40); do
		past
		vacuum
		[[ $(size) -le $(( $(live) * ${1:-120} / 100 )) ]] && return 0
	done
}
# the rounds of a table that is written: segments arrive, the merge writes its output above the segments it replaces, the buffer that ends the file rotates
fragment() {
	local k
	sql primary "DROP INDEX IF EXISTS t_idx" >/dev/null
	sql primary "SET sift.async_maintenance = off" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp)" >/dev/null || bail "create failed"
	for k in 1 2 3 4; do
		sql primary "SET sift.async_maintenance = off" "INSERT INTO t SELECT id + $k * 10000000, grp, amount, pad FROM t WHERE id < 10000000 AND id % 5 = $k - 1" "SELECT sift.col_promote('t_idx')" >/dev/null
	done
	sql primary "SELECT sift.col_merge('t_idx', true)" >/dev/null
	past
	sql primary "INSERT INTO t SELECT id + 90000000 + $RANDOM, grp, amount, pad FROM t WHERE id < 1000" "SELECT sift.col_promote('t_idx')" >/dev/null
	past
}
answer() { sql "$1" "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5"; }
table_answer() { sql primary "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SET sift.enable_col_scan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5"; }
replayed_within() {
	local lsn
	lsn=$(sql primary "SELECT pg_current_wal_insert_lsn()")
	sleep "$1"
	[[ $(sql standby "SELECT pg_last_wal_replay_lsn() >= '$lsn'") == t ]]
}
# a standby query on the table that stays open until release (or $1 seconds)
hold() {
	rm -f "$D/held" "$D/unhold"
	{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t WHERE id < 10;"; echo "\\! touch '$D/held'"; echo "\\! n=0; while [ ! -e '$D/unhold' ] && [ \$n -lt $(( $1 * 20 )) ]; do sleep 0.05; n=\$((n + 1)); done"
	  echo "SET enable_seqscan = off; SELECT 'after', count(*), sum(amount) FROM t WHERE grp = 5;"; echo "COMMIT;"; } | sql standby >"$D/hold.out" 2>&1 &
	HOLDER=$!
	until [[ -e $D/held ]]; do sleep 0.05; done
}
release() { touch "$D/unhold"; wait "$HOLDER"; }
held_answer_ok() { test "$(grep after "$D/hold.out" | tr -d ' ')" == "after|$(answer primary)"; }
no_failure() { ! grep -q -E 'ERROR|pgbench: error|failed transactions: [1-9]' "$1"; }
verified() { test -z "$(sql "$1" "SELECT sift.col_verify('t_idx', true)" 2>&1)"; }
agree() { test "$(answer primary)" == "$(table_answer)"; }
recovered() { verified primary && agree; }

# 1. a merge leaves the file twice the live bytes with the last block live: the shrink brings it back, a standby query on the table running
fragment
check "the merges leave the file over 1.6 times the live bytes" loose
size_before=$(size)
hold 45
vacuum_until
check "a standby query holds the table: the file is within 1.2 times the live bytes" tight
check "... it did not grow meanwhile" test "$(size)" -le "$size_before"
check "... the replay does not stall behind the query" replayed_within 3
release
check "... the query reads the same rows as the primary" held_answer_ok
check "... the index counts as the table" agree
catchup primary standby
check "... the primary verifies the index" verified primary
check "... and the standby" verified standby

# 2. inserts, deletes, merges and scans while the VACUUMs shrink, and a standby query
fragment
hold 30
printf 'INSERT INTO t SELECT -g - %s, g %% 500, 1, $$w$$ FROM generate_series(1, 20) g;\n' "$RANDOM" >"$D/ins.sql"
printf '\\set g random(0, 499)\nSELECT count(*), sum(amount) FROM t WHERE grp = :g;\n' >"$D/rd.sql"
printf 'DELETE FROM t WHERE id = -(1 + (random() * 100000)::int);\n' >"$D/del.sql"
pgbench_bg primary 4 8 -f "$D/ins.sql@2" -f "$D/rd.sql@3" -f "$D/del.sql@1"
for _ in 1 2 3 4 5; do sleep 1; sql primary "SET sift.async_maintenance = off" "SELECT sift.col_promote('t_idx')" "SELECT sift.col_merge('t_idx')" "VACUUM t" >/dev/null; done
wait "$PGBENCH_PID"
release
check "churn: no statement failed" no_failure "$D/pgbench.log"
catchup primary standby
check "... the index counts as the table" agree
check "... the standby verifies the index" verified standby
vacuum_until
check "... and the file is within 1.2 times the live bytes in the end" tight

# 3. a kill when the pages are copied (phase 5) and when the move is published (phase 6)
for phase in 5 6; do
	fragment
	SIFT_TMP=$(dir primary) leak_expect 1 "terminated by signal 9"
	sql primary "SET sift.async_maintenance = off" "SET sift.test_space_crash = $phase" "VACUUM t" >/dev/null 2>&1
	for _ in $(seq 100); do [[ -f $D/data/postmaster.pid ]] || break; sleep 0.1; done
	node_start primary || bail "primary did not restart"
	check "a kill in phase $phase: the primary verifies the index and counts as the table" recovered
	vacuum_until
	check "... the next VACUUMs bring the file within 1.2 times the live bytes" tight
	catchup primary standby
	check "... the standby verifies the index" verified standby
done
# 4. an INSERT takes the free runs a pass was going to move into: the pass has reserved the header page of a segment and waits (sift.test_shrink_gate, a testing build)
# for an advisory lock; the rows go into the lowest free runs; the pass goes on, reserving what is left, and ends without an error
gate_pass() { # LOCKKEY SETTING STATEMENT... : a VACUUM that waits at the gate while the statements run, a testing build; prints what the VACUUM said
	local key=$1 setting=$2 hp vp s
	shift 2
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(20983, $key);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql primary >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql primary "LOAD 'pg_sift'" "SET sift.async_maintenance = off" "SET $setting" "VACUUM t" >"$D/gate.out" 2>&1 &
	vp=$!
	for _ in $(seq 400); do [[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break; sleep 0.05; done
	[[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] || { touch "$D/release"; bail "the pass did not wait at the gate: $(cat "$D/gate.out")"; }
	for s in "$@"; do sql primary "$s" >/dev/null; done
	touch "$D/release"
	wait "$hp" "$vp"
}
fragment
past
gate_pass 3 "sift.test_shrink_gate = on" "INSERT INTO t SELECT id + 95000000, grp, amount, pad FROM t WHERE id < 120000"
check "rows take the free runs a pass planned: the pass ends without an error or a warning" bash -c "! grep -q -E 'ERROR|WARNING|failed' '$D/gate.out'"
check "... the index counts as the table and verifies" recovered
vacuum_until
check "... and the next VACUUMs bring the file within 1.2 times the live bytes" tight

# 5. a reader holds the old header across the move: a scan that waits (sift.test_vis_pause) after it opened the index while a DELETE and the VACUUMs run; the blocks that left stay
# held by its snapshot, and it reads its rows
fragment
past
want=$(answer primary)
size_held=$(size)
rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 1);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql primary >/dev/null &
HP=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql primary "LOAD 'pg_sift'" "SET sift.test_vis_pause = 1" "SET enable_seqscan = off" "SELECT count(*), sum(amount) FROM t WHERE grp = 5" >"$D/reader.out" 2>&1 &
RP=$!
for _ in $(seq 400); do [[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break; sleep 0.05; done
[[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] || { touch "$D/release"; bail "the scan did not wait: $(cat "$D/reader.out")"; }
sql primary "DELETE FROM t WHERE grp = 5" >/dev/null
for _ in 1 2 3; do sql primary "SELECT pg_current_xact_id()" >/dev/null; vacuum; done
held=$(sql primary "SELECT pending_blocks FROM sift.col_space('t_idx')")
file_held=$(size)
touch "$D/release"
wait "$HP" "$RP"
check "a reader holds the index across the passes: the blocks that left stay pending" test "$held" -gt 0
check "... the file is not cut under it" test "$file_held" -ge "$size_held"
check "... and the reader reads the rows of its snapshot" test "$(tail -1 "$D/reader.out" | tr -d ' ')" == "$(tr -d ' ' <<<"$want")"
vacuum_until
check "... once it is done the file is within 1.2 times the live bytes" tight

# 6. the catalog: every run of the file and the free space, on the primary, on the standby and for a role of pg_monitor that is no superuser
sql primary "CREATE ROLE watcher LOGIN" "GRANT pg_monitor TO watcher" >/dev/null
catchup primary standby
as_watcher() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir "$1")/sock" -p "$(port "$1")" -U watcher -d postgres --command="$2"; }
space_row="SELECT used_blocks + reusable_blocks + pending_blocks = high_water AND relation_blocks >= high_water AND largest_free_extent <= reusable_blocks + pending_blocks AND tail_what IS NOT NULL AND tail_end <= high_water AND shrink_to <= high_water AND shrink_blocks >= 0 FROM sift.col_space('t_idx')"
tiles="SELECT sum(nblocks) = (SELECT high_water FROM sift.col_space('t_idx')) FROM sift.col_extents('t_idx')"
check "sift.col_space on the primary adds up to the high-water mark" test "$(sql primary "$space_row")" == t
check "sift.col_extents and sift.col_space on the standby add up too" test "$(sql standby "$space_row")|$(sql standby "$tiles")" == "t|t"
check "a member of pg_monitor, no superuser, reads both on the standby" test "$(as_watcher standby "$space_row")|$(as_watcher standby "$tiles")|$(as_watcher standby "SELECT rolsuper FROM pg_roles WHERE rolname = current_user")" == "t|t|f"
check "... and on the primary" test "$(as_watcher primary "$space_row")|$(as_watcher primary "$tiles")" == "t|t"
finish
