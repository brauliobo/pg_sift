#!/usr/bin/env bash
# The before and after hooks of the leak gate (scripts/leakcheck.sh leak_before, leak_after, leak_cluster) on planted leaks: every driver runs the same pair around its unit
# of work (scripts/pgtest.sh regress: a test), so a unit that leaves a resource behind must fail it with the name of what leaked, and a clean unit must pass.
# sift._test_leak leaves each kind on purpose. A palloc chunk in TopMemoryContext is the memcheck gate's (scripts/valgrind.sh --leaks-selftest), a malloc leak
# LeakSanitizer's (test/t/leakcheck_gate.sh): neither is visible to a hook that reads the server log and the shared state. The deliberate findings of this node are set aside
# before finish, which runs the gate on the node like on any other and must find it clean.
. "$(dirname "$0")/lib.bash"

D=$(dir hooks)
plan 12
node_init hooks 1
node_start hooks || bail "start failed"
sql hooks "CREATE DATABASE mut" >/dev/null || bail "setup failed"
msql() { PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$D/sock" -p "$(port hooks)" -U postgres -d mut "$@"; }
msql -c "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
LOG=$D/postgres.log
export SIFT_TMP=$D

# unit KIND: a unit of work that plants KIND, between the hooks; the hooks' own output
unit() {
	leak_before "unit-$1" "$LOG"
	msql -c "SELECT sift._test_leak('$1')" >/dev/null 2>&1
	leak_after "unit-$1" "$D/data" mut 2>&1
}
fails_naming() { local out; ! out=$(unit "$1") && grep -qE -e "$2" <<<"$out"; }

leak_before unit-clean "$LOG"
msql -c "CREATE TABLE t AS SELECT g FROM generate_series(1, 100) g" -c "SELECT count(*) FROM t" -c "DROP TABLE t" >/dev/null
check "a unit that leaks nothing passes the hooks" leak_after unit-clean "$D/data" mut

for spec in "buffer:resource was not closed: \[[0-9]+\]" "relcache:resource was not closed: relation" "tupledesc:resource was not closed: TupleDesc" \
	"snapshot:resource was not closed: snapshot" "lock:you don.t own a lock" "temp:resource was not closed: File" \
	"dsm:resource was not closed: dynamic shared memory" "context:sift leak: memory context \"sift leaked\""; do
	check "the hooks fail a unit that leaks a ${spec%%:*}" fails_naming "${spec%%:*}" "${spec#*:}"
done

# a shared table that a backend counted and never gave back shows when no session is left, at the end of the cluster
msql -c "SELECT sift._test_dsm_join('leak_probe', 4096)" -c "SELECT sift._test_leak('shared')" >/dev/null || bail "planting the shared table failed"
cluster_out() { PGHOST=$D/sock PGPORT=$(port hooks) PGUSER=postgres leak_cluster "$D/data" 2>&1; }
shared_fails() { local out; ! out=$(cluster_out) && grep -q "leak_probe.* is mapped by 1 backend" <<<"$out"; }
check "the cluster check fails a shared table that a backend still counts" shared_fails

# the next unit is clean again: a finding of an earlier unit is not read as its own
leak_before unit-after "$LOG"
msql -c "SELECT 1" >/dev/null
check "a clean unit passes after a leaking one" leak_after unit-after "$D/data" mut

says_fix() { local out; out=$(unit buffer); grep -q "do not allow-list" <<<"$out"; }
check "the failure says to fix the leak and not to allow-list it" says_fix

node_stop hooks
mv "$LOG" "$D/postgres.deliberate"
mv "$D/leakcheck.findings" "$D/findings.deliberate"
finish
