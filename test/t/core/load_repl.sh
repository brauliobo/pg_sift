#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The replication suite of the load gate (scripts/load/run.py --suite repl, scripts/load/suites/repl.py): a hot standby read under the writes, VACUUM and maintenance of the
# primary, caught up, compared and promoted; and logical decoding of a sift_table to a subscriber through toasted bulk inserts, churn, TRUNCATE and changes of the publication.
# A primary with wal_level logical and a hot standby; the case that promotes the standby clones a new one (restandby) for the case after it. One check a case; a case that
# names a finding (open=S107-NN) is a TODO while it fails and fails when it stops. Runs on Format 2.0 and Format 1.0, the release library and Assert-PG.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "$TAP_LIB/load.bash"

node_init lp 1 "shared_preload_libraries = 'pg_sift'
autovacuum = off
wal_level = logical
max_wal_senders = 8
max_replication_slots = 8
max_connections = 80
max_worker_processes = 24
sift.maintenance_naptime = 200
deadlock_timeout = '100ms'
log_min_messages = warning"
node_start lp || bail "start failed"

standby() { node_clone lp ls 2 && node_start ls; }
# restandby: the leak check of the standby ls into LS_LEAKS (its log, its sanitizer reports and its shutdown, which the clone erases; the runner scans the file), its old
# slot dropped once its walsender has gone, and a new clone of lp
restandby() {
	node_leakcheck ls >>"$LS_LEAKS" 2>&1 || echo "leaked server state: the leak check of the standby ls failed before its clone" >>"$LS_LEAKS"
	until [[ $(sql lp "SELECT count(*) FROM pg_replication_slots WHERE slot_name = 'ls' AND active") == 0 ]]; do sleep 0.05; done
	sql lp "SELECT count(pg_drop_replication_slot(slot_name)) FROM pg_replication_slots WHERE slot_name = 'ls'" >/dev/null
	standby
}
standby || bail "the standby did not start"
export -f $(compgen -A function)
export TAPDIR TAP_SCRIPTS SIFT_PORT LS_LEAKS=$TAPDIR/ls.leaks
: >"$LS_LEAKS"
S=$(dir ls)
LOAD_STANDBY_HOST=$S/sock LOAD_STANDBY_PORT=$(port ls) LOAD_PROMOTE="pg_ctl -D '$S/data' -w promote >/dev/null" LOAD_RESTANDBY=". '$TAP_SCRIPTS/leakcheck.sh'; restandby" \
	load_suite lp --suite repl --log "$S/postgres.log" --log "$LS_LEAKS"
done_testing
finish
