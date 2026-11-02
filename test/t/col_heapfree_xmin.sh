#!/usr/bin/env bash
# Reads of a heap sift_col index with track_updates and track_deletes that do without the heap: the rows of a segment whose inserters the stamp says committed
# before the snapshot's xmin and of a write buffer whose records say the same, with the tombstone log for what was deleted. What the single-session test (col_heapfree_xmin)
# cannot: a snapshot older than the commits, a transaction in doubt (prepared), a backend that keeps its cache of the log while VACUUM gives the addresses to new rows, and a
# hot standby. Every answer is compared with the same aggregate with the heap checking every page and with no sift plan at all; the stamp mutant must be wrong where a stamp is needed.
. "$(dirname "$0")/lib.bash"

plan 25
ROWS=$(knob SIFT_HF_ROWS 20000 12000)
CONF="max_prepared_transactions = 5
autovacuum = off
synchronous_commit = on"
node_init p 28 "$CONF"
node_start p || bail "start failed"
D=$(dir p)

sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)" \
	"INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id', track_deletes = on)" \
	"VACUUM (FREEZE, ANALYZE) events" >/dev/null || bail "setup failed"
node_clone p s 29 "max_prepared_transactions = 5"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

Q="SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id"
A="SELECT md5(coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), 'none')) FROM ($Q) q"
MODE_free="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = off"
MODE_checked="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = on"
MODE_oracle="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off; SET sift.test_no_heapfree = off"
MODE_stamp="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = off; SET sift.test_hf_ignore_stamp = on"
# ans NODE MODE: the answer; pages NODE MODE: the heap pages the plan read
ans() { local m=MODE_$2; sql "$1" "${!m}" "$A"; }
pages() { local m=MODE_$2; sql "$1" "${!m}" "EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) $Q" | grep '"Heap Pages"' | tr -dc 0-9; }
plan_has() { local m=MODE_$2; sql "$1" "${!m}" "EXPLAIN (COSTS OFF) $Q" | grep -q SiftAgg; }
agree() { local f=$(ans "$1" free) c=$(ans "$1" checked) o=$(ans "$1" oracle); [[ $f == "$c" && $f == "$o" ]] || { diag "$1: free $f checked $c oracle $o"; return 1; }; }
stamped() { sql p "SELECT count(stamp) FROM sift.col_stamps('events_c')"; }
segs() { sql p "SELECT count(*) FROM sift.col_stamps('events_c')"; }

plan_ok() { plan_has p free && ! plan_has p oracle; }
check "the plans: the aggregate is a SiftAgg plan with sift, not without" plan_ok
check "free, checked and oracle agree on the fresh index" agree p
check "a fresh index reads no heap page (visible after the VACUUM), the stamp is on its segment" test "$(pages p free)" == 0 -a "$(stamped)" == "$(segs)"

# a snapshot older than the commits, kept open until released: it must see the old rows, and the heap-free read must not take the new segment for visible to it
{ cat <<EOF
BEGIN ISOLATION LEVEL REPEATABLE READ;
$A;
\\! touch '$D/locked'; while [ ! -e '$D/release' ]; do sleep 0.05; done
\\o $D/held.out
SET sift.enable_agg = on; SET sift.test_no_heapfree = off;
$A;
SET sift.test_no_heapfree = on;
$A;
SET sift.enable_agg = off; SET sift.test_no_heapfree = off;
$A;
SET sift.enable_agg = on;
SET sift.test_hf_ignore_stamp = on;
$A;
\\o
COMMIT;
EOF
} | sql p >"$D/held.base" &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
OLD=$(ans p oracle)
sql p "UPDATE events SET counter_a = counter_a + 5 WHERE id % 11 = 0" "DELETE FROM events WHERE id % 13 = 0" \
	"INSERT INTO events SELECT i, i % 10, i, 'n' FROM generate_series($ROWS + 1, $ROWS + 400) i" \
	"UPDATE events SET id = id + 100000 WHERE id % 17 = 0" "SELECT sift.col_promote('events_c')" >/dev/null
touch "$D/release"
wait "$hp"
check "the old snapshot reads the old rows free, checked and without sift" test "$(grep -c "^$(sed -n 1p "$D/held.base" | tr -d ' ')$" "$D/held.out")" == 3
check "the stamp mutant counts the rows committed after that snapshot: the stamp is what keeps them out" test "$(grep -c "^$(sed -n 1p "$D/held.base" | tr -d ' ')$" "$D/held.out")" == 3 -a "$(tail -1 "$D/held.out" | tr -d ' ')" != "$(sed -n 1p "$D/held.base" | tr -d ' ')"
check "a new snapshot: free, checked and oracle agree, and it is not the old answer" agree p
check "the new snapshot reads no heap page where the old one needed them" test "$(pages p free)" == 0 -a "$(pages p checked)" -gt 0

# a transaction in doubt: its records are in the buffer with an xid that is not finished; the rows of a segment it is in say nothing
sql p "BEGIN; INSERT INTO events SELECT i, i % 10, i, 'p' FROM generate_series($ROWS + 1001, $ROWS + 1300) i; UPDATE events SET counter_a = counter_a + 9 WHERE id % 29 = 0; DELETE FROM events WHERE id % 31 = 0; PREPARE TRANSACTION 'hf1'" >/dev/null
check "prepared, not committed: every read is the committed rows" agree p
sql p "SELECT sift.col_promote('events_c')" >/dev/null
check "promoted with the transaction in doubt: still the committed rows" agree p
check "that segment says nothing, the others do" test "$(stamped)" -lt "$(segs)" -a "$(stamped)" -gt 0
check "and the heap is read for it" test "$(pages p free)" -gt 0
sql p "COMMIT PREPARED 'hf1'" >/dev/null
check "committed: every read is the rows with the transaction's changes" agree p
check "the segment written in doubt stays silent until it is rebuilt, and the answers do not depend on it" test "$(stamped)" -lt "$(segs)"
sql p "REINDEX INDEX events_c" >/dev/null
rebuilt_ok() { [[ $(stamped) == $(segs) && $(pages p free) == 0 ]] && agree p; }
check "rebuilt: every segment says its inserters committed, no heap page is read, and the answers agree" rebuilt_ok

# a backend that read the log keeps it while VACUUM gives the addresses to new rows: one session answers on request, each time with the heap-free read and with no sift plan
cat >"$D/live.sql" <<EOF
\\set QUIET on
\\pset tuples_only on
\\pset format unaligned
SET sift.test_no_heapfree = off;
\\! touch '$D/live.ready'
EOF
for n in 1 2 3; do
	cat >>"$D/live.sql" <<EOF
\\! while [ ! -e '$D/req$n' ]; do sleep 0.05; done
SET sift.enable_agg = on;
\\o $D/live$n.free
$A;
\\o
SET sift.enable_agg = off;
\\o $D/live$n.oracle
$A;
\\o
\\! touch '$D/done$n'
EOF
done
psql "$(conninfo p)" -X -q -f "$D/live.sql" >"$D/live.log" 2>&1 &
lp=$!
until [[ -e $D/live.ready ]]; do sleep 0.05; done
touch "$D/req1"; until [[ -e $D/done1 ]]; do sleep 0.05; done
check "the long-lived backend, first read: free equals the oracle" test "$(tr -d ' ' <"$D/live1.free")" == "$(tr -d ' ' <"$D/live1.oracle")" -a -s "$D/live1.free"
sql p "DELETE FROM events WHERE id % 7 = 0" "VACUUM events" "INSERT INTO events SELECT i, i % 10, i, 'v' FROM generate_series($ROWS + 2001, $ROWS + 3500) i" >/dev/null
touch "$D/req2"; until [[ -e $D/done2 ]]; do sleep 0.05; done
check "the same backend after the deletes, VACUUM and new rows at the old addresses: free equals the oracle" test "$(tr -d ' ' <"$D/live2.free")" == "$(tr -d ' ' <"$D/live2.oracle")" -a -s "$D/live2.free"
sql p "UPDATE events SET counter_a = counter_a + 3 WHERE id % 5 = 0" "SELECT sift.col_promote('events_c')" "DELETE FROM events WHERE id BETWEEN $ROWS + 2001 AND $ROWS + 2300" "VACUUM events" "INSERT INTO events SELECT i, i % 10, i, 'w' FROM generate_series($ROWS + 4001, $ROWS + 4800) i" >/dev/null
touch "$D/req3"; until [[ -e $D/done3 ]]; do sleep 0.05; done
check "and after a second round: free equals the oracle" test "$(tr -d ' ' <"$D/live3.free")" == "$(tr -d ' ' <"$D/live3.oracle")" -a -s "$D/live3.free"
wait "$lp"
check "the backend ended cleanly" test ! -s "$D/live.log"

# the hot standby replays the log, the directory with its stamps and the heap
catchup p s || bail "standby catchup failed"
check "standby: free, checked and oracle agree, and the answer is the primary's" agree s
check "standby: the answer is the primary's" test "$(ans s free)" == "$(ans p oracle)"
check "standby: the heap is not read for what the primary's log settles" test "$(pages s free)" -lt "$(pages s checked)"

# the control: track_deletes off, the answers are the same and the heap is read again
sql p "ALTER INDEX events_c RESET (track_deletes)" "DELETE FROM events WHERE id % 41 = 0" >/dev/null
check "track_deletes off: free equals the oracle, the heap is read" agree p
check "track_deletes off: pages are read" test "$(pages p free)" -gt 0
sql p "ALTER INDEX events_c SET (track_deletes = on)" >/dev/null
check "track_deletes on again: free equals the oracle and the heap pages go away" agree p
check "track_deletes on again: no heap page is read" test "$(pages p free)" == 0

finish
