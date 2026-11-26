#!/usr/bin/env bash
# The rebuild slot of a write buffer's arena (one reader builds the entries from the heap, the others wait): the reader that owns it releases it when its statement ends in an error,
# wherever the error falls after it took the slot, and the readers that wait for it neither poll nor wait for the backend to exit. The owner stops at a test gate right after it took the
# slot (an advisory lock a test holds, so no timing is involved) and is cancelled there; a second reader waits for the slot meanwhile.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init rr 33 "sift.test_create_format1 = on
autovacuum = off
sift.async_maintenance = off
max_parallel_workers_per_gather = 0"
node_start rr || bail "start failed"
D=$(dir rr)
sql rr "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE a (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = off)" \
	"CREATE INDEX a_ix ON a USING sift_col (id, g, v)" \
	"INSERT INTO a SELECT i, i % 10, i FROM generate_series(1, 3000) i" >/dev/null || bail "setup failed"
crash rr
node_start rr || bail "restart after the crash failed"

READ="SELECT count(*) || ',' || sum(v) FROM a"
HEAP=$(sql rr "SET sift.enable_agg = off" "$READ" | tail -1)
reader() { sql rr "LOAD 'pg_sift'" "SET application_name = '$1'" "SET sift.agg_force = on" "$2" "$READ"; }
backend() { sql rr "SELECT pid FROM pg_stat_activity WHERE application_name = '$1'"; }
waiting_on() { [[ $(sql rr "SELECT count(*) FROM pg_stat_activity WHERE application_name = '$1' AND wait_event = '$2'") == 1 ]]; }
switches() { awk '/^voluntary_ctxt_switches/ {print $2}' "/proc/$1/status"; }

rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 11);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql rr >/dev/null &
holder=$!
wait_for "[[ -e $D/locked ]]" 30

# the owner's session lives on after its error (it does not exit and release the slot with the backend)
{ echo "LOAD 'pg_sift'; SET application_name = 'owner'; SET sift.agg_force = on; SET sift.test_rebuild_gate = on;"; echo "$READ;"; echo "\\! touch '$D/owner_failed'"
  echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -h "$D/sock" -p "$(port rr)" -U postgres -d postgres >"$D/owner.out" 2>&1 &
owner=$!
wait_for "waiting_on owner advisory" 60
check "the reader that took the slot stops at the gate" waiting_on owner advisory

reader waiter "SELECT 1" >"$D/waiter.out" 2>&1 &
waiter=$!
wait_for "waiting_on waiter SiftArenaWait" 60
check "a second reader waits for the slot" waiting_on waiter SiftArenaWait
WPID=$(backend waiter)
S1=$(switches "$WPID")
sleep 1
S2=$(switches "$WPID")
check "the waiting reader sleeps until the slot is released ($((S2 - S1)) wakeups in a second)" test "$((S2 - S1))" -lt 20

sql rr "SELECT pg_cancel_backend($(backend owner))" >/dev/null
wait_for "[[ -e $D/owner_failed ]]" 60
check "the owner is cancelled at the gate and its session goes on" grep -q "canceling statement" "$D/owner.out"
wait_for "! kill -0 $waiter 2>/dev/null" 60
check "the cancelled owner released the slot: the waiting reader reads the rows" test "$(cat "$D/waiter.out" | tail -1)" == "$HEAP"
touch "$D/release"; wait "$holder" "$owner"
check "a reader after them reads the rows" test "$(reader later 'SELECT 1' | tail -1)" == "$HEAP"
finish
