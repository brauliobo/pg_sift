#!/usr/bin/env bash
# A segment promoted while a transaction that inserted some of its rows had not finished waits for it (a pending stamp, SIFT_SEG_XPENDING) and says its inserters committed once they all
# have. What the single-session test (col_heapfree_pending) cannot show: an inserter that dies with its backend (a power loss, kill -9: nothing says it rolled back, the recovery finds it
# did not commit), the pending stamp that crash recovery and WAL replay give back as it was, and a hot standby whose snapshot is older than the commit that settled the stamp. Every
# answer is compared with the same aggregate with the heap checking every page and with no sift plan at all.
. "$(dirname "$0")/lib.bash"

plan 18
ROWS=$(knob SIFT_HF_ROWS 20000 12000)
CONF="max_prepared_transactions = 5
autovacuum = off
synchronous_commit = on"
node_init p 34 "$CONF"
node_start p || bail "start failed"
D=$(dir p)

sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)" \
	"INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) events" >/dev/null || bail "setup failed"
node_clone p s 35 "max_prepared_transactions = 5
hot_standby_feedback = on"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

Q="SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id"
A="SELECT md5(coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), 'none')) FROM ($Q) q"
MODE_free="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = off"
MODE_checked="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = on"
MODE_oracle="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off; SET sift.test_no_heapfree = off"
ans() { local m=MODE_$2; sql "$1" "${!m}" "$A"; }
pages() { local m=MODE_$2; sql "$1" "${!m}" "EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) $Q" | grep '"Heap Pages"' | tr -dc 0-9; }
agree() { local f=$(ans "$1" free) c=$(ans "$1" checked) o=$(ans "$1" oracle); [[ $f == "$c" && $f == "$o" ]] || { diag "$1: free $f checked $c oracle $o"; return 1; }; }
stamped() { sql "${1:-p}" "SELECT count(stamp) FROM sift.col_stamps('events_c')"; }
segs() { sql "${1:-p}" "SELECT count(*) FROM sift.col_stamps('events_c')"; }
stamps() { sql "${1:-p}" "SELECT coalesce(string_agg(gen || ':' || entries || ':' || coalesce(stamp::text, '-'), ' ' ORDER BY gen), '') FROM sift.col_stamps('events_c')"; }
count_rows() { sql "$1" "SET sift.enable_agg = off" "SELECT count(*) FROM events WHERE id > $ROWS"; }

check "the fresh index: every segment says its inserters committed" test "$(stamped)" == "$(segs)"

# an inserter that dies with its backend: its records are in the buffer and the promotion waits for it
{ cat <<EOF
BEGIN;
INSERT INTO events SELECT i, i % 10, i, 'dead' FROM generate_series($ROWS + 1, $ROWS + 400) i;
\\! touch '$D/open'
SELECT pg_sleep(600);
EOF
} | sql p >"$D/open.out" 2>&1 &
until [[ -e $D/open ]]; do sleep 0.05; done
sql p "INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series($ROWS + 1001, $ROWS + 1300) i" "SELECT sift.col_promote('events_c')" >/dev/null
check "promoted while the inserter is open: the segment waits, and the three reads agree" test "$(stamped)" -lt "$(segs)" -a "$(stamped)" -gt 0
check "... and the open inserter's rows are in no answer" agree p
BEFORE=$(stamps p)
catchup p s || bail "standby catchup failed"
check "the standby has the stamps the primary wrote (replay of the pending entry)" test "$(stamps s)" == "$BEFORE"
crash p
node_start p || bail "primary restart failed"
check "after the crash recovery the entry is as it was (WAL replay gives back the pending stamp)" test "$(stamps p)" == "$BEFORE"
check "the inserter did not commit: none of its rows is visible, every read agrees" agree p
check "the dead inserter's rows are not counted (only the committed ones are)" test "$(count_rows p)" == 300
sql p "SELECT sift.col_promote('events_c')" >/dev/null
check "a maintenance step after the recovery finds it rolled back: the segment says nothing, as one that never had a stamp" test "$(stamped)" -lt "$(segs)"
check "... and the reads still agree and the heap is read for it" test "$(pages p free)" -gt 0 -a "$(count_rows p)" == 300
catchup p s || bail "standby catchup failed"
check "the standby reads the rows the committed transactions wrote, and not the dead one's" agree s

# a standby snapshot older than the commit that settles a stamp must not read the segment as stamped: the stamp is the xid after the inserters, and its xmin is below it
sql p "REINDEX INDEX events_c" >/dev/null
catchup p s || bail "standby catchup failed"
{ cat <<EOF
BEGIN ISOLATION LEVEL REPEATABLE READ;
$A;
\\! touch '$D/standby_locked'; while [ ! -e '$D/standby_release' ]; do sleep 0.05; done
\\o $D/standby.out
SET sift.enable_agg = on; SET sift.test_no_heapfree = off;
$A;
SET sift.test_no_heapfree = on;
$A;
SET sift.enable_agg = off; SET sift.test_no_heapfree = off;
$A;
\\o
COMMIT;
EOF
} | sql s >"$D/standby.base" &
sp=$!
until [[ -e $D/standby_locked ]]; do sleep 0.05; done
sql p "BEGIN; INSERT INTO events SELECT i, i % 10, i, 'p' FROM generate_series($ROWS + 2001, $ROWS + 2300) i; PREPARE TRANSACTION 'pend1'" >/dev/null
sql p "INSERT INTO events SELECT i, i % 10, i, 'c' FROM generate_series($ROWS + 3001, $ROWS + 3300) i" "SELECT sift.col_promote('events_c')" >/dev/null
check "a promotion with a prepared transaction in doubt waits for it" test "$(stamped)" -lt "$(segs)"
sql p "COMMIT PREPARED 'pend1'" "SELECT sift.col_promote('events_c')" >/dev/null
check "committed, the next promotion settles it: every segment of the primary says its inserters committed" test "$(stamped)" == "$(segs)"
catchup p s || bail "standby catchup failed"
touch "$D/standby_release"
wait "$sp"
check "the standby snapshot older than the commit reads the old rows in the three modes: the settled stamp is not taken for visible to it" \
	test "$(tr -d ' ' <"$D/standby.out" | sort -u | wc -l)" == 1 -a "$(tr -d ' ' <"$D/standby.out" | sed -n 1p)" == "$(sed -n 1p "$D/standby.base" | tr -d ' ')"
check "the standby has the settled stamps" test "$(stamps s)" == "$(stamps p)"
check "a new snapshot on the standby: free, checked and oracle agree, with the committed rows" agree s
check "... and it reads fewer heap pages than the checked read, as the primary's stamps say" test "$(pages s free)" -lt "$(pages s checked)"
check "the primary: the three reads agree and no heap page is read" agree p
check "... no heap page is read once the stamps are settled" test "$(pages p free)" == 0

finish
