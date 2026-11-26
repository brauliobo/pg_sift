#!/usr/bin/env bash
# The bytes-per-row gate (scripts/bpr/run.py, cases.py, fixtures.sql): the disk and the WAL a row costs on each storage path of sift, in windows that start after a CHECKPOINT of a
# cluster with full_page_writes on and wal_compression off, so that a bulk load, a build, a write or a VACUUM that costs 3.5 times what it did fails here even when its ratio to
# the last run does not. One BPR line a case (a comment, as the job log of a passing test keeps comments) and one check each. A case that names a known defect (open=W-NN)
# is a TODO while it fails its ceilings and fails when it stops: its row is then made clean. BPR_ONLY=RE runs the cases whose name matches.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

node_init bpr 1 "shared_preload_libraries = 'pg_sift'
wal_level = replica
full_page_writes = on
wal_compression = off
autovacuum = off
sift.async_maintenance = off
checkpoint_timeout = 1h
max_wal_size = 4GB
min_wal_size = 1GB
wal_writer_delay = 10ms
max_parallel_maintenance_workers = 0
max_parallel_workers_per_gather = 0
maintenance_work_mem = 16MB
work_mem = 4MB"
node_start bpr || bail "start failed"

bpr() { PGHOST=$(dir bpr)/sock PGPORT=$(port bpr) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/bpr/run.py" --dir "$(dir bpr)" ${BPR_ONLY:+--only "$BPR_ONLY"} "$@"; }
out=$(bpr 2>"$(dir bpr)/bpr.err") || diag "$(cat "$(dir bpr)/bpr.err")"
diag "$out"
done_testing
check "the runner ran every case" test "$(grep -c '^BPR [a-z]' <<<"$out")" -eq "$(bpr --list | grep -c .)"
while IFS= read -r l; do
	[[ $l == "BPR "[a-z]* ]] || continue
	name=${l#BPR } name=${name%% *} verdict=${l##* }
	case $verdict in
	PASS) ok "$name" ;;
	OPEN=*) NTEST=$((NTEST + 1)); echo "not ok $NTEST - $name # TODO ${verdict#OPEN=}" ;;
	*) not_ok "$name: $l" ;;
	esac
done <<<"$out"
finish
