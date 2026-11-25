#!/usr/bin/env bash
# A worker that the postmaster never starts does not keep its slot for good: with sift.max_maint_workers = 1 and the postmaster stopped, the request of one table takes the slot and its worker
# is registered but never up, a request of another table is refused and says one is running, and after the lease of a slot (sift.test_worker_lease_ms in a testing build) the next request
# takes the slot again; once the postmaster goes on, the workers start and the tables are promoted.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
LEASE_MS=2000
node_init wl 94 "autovacuum = off
log_min_messages = debug1
sift.test_col_net = off
sift.max_maint_workers = 1
sift.test_worker_lease_ms = $LEASE_MS
sift.maintenance_naptime = 200"
node_start wl || bail "start failed"
D=$(dir wl)
TABLE="(id int, key int, kind text, amount numeric(10,2), pad text)"
sql wl "CREATE EXTENSION pg_sift" "ALTER DATABASE postgres SET sift.async_maintenance = on" >/dev/null || bail "setup failed"
for t in t1 t2 t3; do
	sql wl "CREATE TABLE $t $TABLE" "INSERT INTO $t SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
		"CREATE INDEX ${t}_idx ON $t USING sift_col (id, key, kind, amount)" "VACUUM $t" >/dev/null || bail "load of $t failed"
done
change() { printf '%s\n' "UPDATE $1 SET amount = amount + 1 WHERE id % 5 = 0;" "DELETE FROM $1 WHERE id % 100 = 50;" \
	"INSERT INTO $1 SELECT 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g;" "VACUUM $1;"; }
refusals() { grep -c "no background worker for promote of index \"public.$1_idx\"" "$D/postgres.log" || true; }
buffered() { sql wl "SELECT coalesce(sum(rows), 0) FROM sift.col_segments('$1_idx') WHERE kind <> 'immutable'"; }
promoted() { [[ $(buffered $1) == 0 ]]; }
PM=$(head -1 "$D/data/postmaster.pid")
# a stopped postmaster takes no stop request: it goes on before the node is stopped, whatever ends the script
stop_lease_node() { kill -CONT "$PM" 2>/dev/null; cleanup; }
trap stop_lease_node EXIT

# one session, since a stopped postmaster accepts no connection: the first table registers the worker that is never started, the second finds the slot taken, the third comes after the lease
{
	echo "\\! kill -STOP $PM"
	change t1
	change t2
	echo "\\! sleep $((LEASE_MS / 1000 + 2))"
	change t3
	echo "\\! kill -CONT $PM"
} >"$D/steps.sql"
sql wl "\\i $D/steps.sql" >/dev/null || bail "the session failed"

check "a request that finds the slot of a worker not yet up taken is refused, and the detail says one is running" \
	bash -c "[[ $(refusals t2) -ge 1 ]] && grep -A1 'no background worker for promote of index \"public.t2_idx\"' '$D/postgres.log' | grep -q 'sift.max_maint_workers is 1 and 1 are running'"
check "after the lease the slot is taken again: the request of the third table is not refused" test "$(refusals t3)" == 0
served() { for t in t1 t2 t3; do change $t >"$D/more.sql"; sql wl "\\i $D/more.sql" >/dev/null; done; promoted t1 && promoted t2 && promoted t3; }
check "the postmaster starts the workers and the tables are promoted" wait_for served 120
none_up() { [[ $(sql wl "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
check "no worker is left up" wait_for none_up 60
finish
