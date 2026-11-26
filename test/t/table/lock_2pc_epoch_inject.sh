#!/usr/bin/env bash
# A row locker that asked whether a prepared transaction holds rows of a sift_table while a PREPARE TRANSACTION was between its coarse locks and its rows asks again once the rows are in the
# lock manager: the epoch of the prepares moves after the markers of the rows, not before (a locker that asked in between keeps no answer that the prepare outdates). The PREPARE stops at the injection point sift-rowlock-prepare-rows (PostgreSQL built with
# --enable-injection-points: scripts/pg-assert.sh --inject) while a session locks a row FOR KEY SHARE; after it, the same session locks another row FOR UPDATE, and its intention is
# then a page lock of the lock manager, which a restart finds, not a record in shared memory.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }
((PG_MAJOR >= 18)) || { echo "1..0 # SKIP the wait of an injection point is named from PostgreSQL 18"; exit 0; }

plan 4
node_init le 54 "autovacuum = off
sift.async_maintenance = off
sift.test_create_format1 = off
max_prepared_transactions = 4"
node_start le || bail "start failed"
D=$(dir le)
POINT=sift-rowlock-prepare-rows
sql le "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" \
	"CREATE TABLE orders (id int PRIMARY KEY, st text) USING sift_table" \
	"INSERT INTO orders SELECT g, 'new' FROM generate_series(1, 20) g" >/dev/null || bail "setup failed"

# the session b, fed statement by statement: b_run returns once it ran the statement (the sessions started after it hold its input open too, so b ends once they did)
mkfifo "$D/b.in"
PGAPPNAME=b sql le <"$D/b.in" >"$D/b.out" 2>&1 &
b=$!
exec 4>"$D/b.in"
nb=0
b_run() { nb=$((nb + 1)); printf '%s\n\\! touch %s\n' "$1" "$D/b.$nb" >&4; wait_for "[[ -e $D/b.$nb ]]" 30; }
b_page_locks() { sql le "SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a USING (pid) WHERE a.application_name = 'b' AND l.locktype = 'page' AND l.relation = 'orders'::regclass"; }

# another row held keeps the prepared transaction from taking the partition: its row is an entry and a tuple lock with the marker of the relation
tap_tx_hold le holder "BEGIN; SELECT id FROM orders WHERE id = 20 FOR UPDATE"
sql le "SELECT injection_points_set_local()" "SELECT injection_points_attach('$POINT', 'wait')" "BEGIN" "SELECT id FROM orders WHERE id = 1 FOR UPDATE" \
	"PREPARE TRANSACTION 'p1'" >"$D/p1.out" 2>&1 &
p1=$!
check "the PREPARE stops between its coarse locks and its rows" \
	wait_for "[[ \$(sql le \"SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = '$POINT'\") == 1 ]]" 30
check "a row of the table is locked FOR KEY SHARE meanwhile" b_run "BEGIN; SELECT id FROM orders WHERE id = 3 FOR KEY SHARE;"
sql le "SELECT injection_points_wakeup('$POINT')" >/dev/null
wait "$p1" || bail "the PREPARE failed: $(cat "$D/p1.out")"
b_run "SELECT id FROM orders WHERE id = 4 FOR UPDATE;"
check "after the PREPARE, the intention of FOR UPDATE is a page lock of the lock manager" test "$(b_page_locks)" -gt 0
echo "ROLLBACK;" >&4
exec 4>&-
tap_tx_release holder
wait "$b"
check "the prepared row is held: COMMIT PREPARED ends it" sql le "COMMIT PREPARED 'p1'"
finish
