#!/usr/bin/env bash
# The production viability gate (scripts/prod-viability-gate.py): 27 operations of the four lifecycle classes (creations, writes, maintenance and VACUUM, reads, and the cleanup of
# errors and cancels) at 32,768, 65,536 and 131,072 rows, tables built at maintenance_work_mem 64 MB and operated at 1 to 4 MB, in 75 s: the memory a case holds stays flat
# (alpha_mem 1.15, 8 B/row of resident memory and 2 B/row of the memory scopes of sift, 3 budgets + a fixed base), the Format 2.0 operations write no temp file under temp_file_limit
# 1 MB, the spills stay in a multiple of their input, no case leaves a file in pg_ls_tmpdir(), the time grows at most 5.5 times for 4 times the rows, a real autovacuum worker
# honours autovacuum_work_mem 2 MB while maintenance_work_mem is 64 MB, and 3 churns with VACUUM keep the size within twice a clean build.
# One GATE_ROW line a case (a comment, as the job log of a passing test keeps comments) and one check each. PROD_GATE_ONLY=RE runs the cases whose name matches, PROD_GATE_ROWS the
# smallest size, PROD_GATE_LANES the cases that run side by side (2), PROD_GATE_CACHE the sift.backend_cache_kb of the operations (1MB).
# A server built with assertions or run under a sanitizer judges everything but the resident memory (the shadow memory and the checks of an allocation move it).
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
NODE_CONF="autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
restart_after_crash = on
sift.async_maintenance = off"
node_init pv 0 "$NODE_CONF"
node_init pvav 1 "$NODE_CONF
autovacuum = on
autovacuum_naptime = 1s
autovacuum_max_workers = 2
autovacuum_vacuum_threshold = 2000000000
autovacuum_vacuum_insert_threshold = -1
autovacuum_analyze_threshold = 2000000000
autovacuum_vacuum_scale_factor = 0
autovacuum_freeze_max_age = 100000
autovacuum_vacuum_cost_delay = 0
maintenance_work_mem = 64MB
autovacuum_work_mem = 2MB
temp_file_limit = 1MB
sift.dead_rewrite_pct = 15"
node_start pv || bail "start failed"
node_start pvav || bail "start failed"
for n in pv pvav; do sql "$n" "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"; done

gate() { PGHOST=$(dir pv)/sock PGPORT=$(port pv) PGUSER=postgres PGDATABASE=postgres python3 -I "$REPO/scripts/prod-viability-gate.py" "$@" ${PROD_GATE_ONLY:+--only "$PROD_GATE_ONLY"}; }
args=(--dir "$(dir pv)" --summary-tsv "$(dir pv)/summary.tsv" --av-host "$(dir pvav)/sock" --av-port "$(port pvav)" --av-log "$(dir pvav)/postgres.log")
if [[ $(sql pv "SHOW debug_assertions") == on || -n ${SIFT_PRELOAD:-} ]]; then
	args+=(--no-rss)
	diag "assertions or sanitizer: the resident memory is not judged, the memory scopes, the temp files, the disk and the autovacuum worker are"
fi
out=$(gate run "${args[@]}" 2>"$(dir pv)/gate.err")
diag "$(grep '^GATE_' <<<"$out")"
diag "$(cat "$(dir pv)/gate.err")"
diag "gate wall time: ${SECONDS} s"
rows=$(grep '^GATE_ROW ' <<<"$out")
done_testing
check "every case ran" test "$(grep -c . <<<"$rows")" -eq "$(gate list | grep -c .)"
while IFS= read -r l; do
	[[ -n $l ]] || continue
	name=${l#GATE_ROW } name=${name%% |*}
	if [[ $l == *" | PASS" ]]; then ok "$name"; else not_ok "$name: ${l##* | }"; fi
done <<<"$rows"
finish
