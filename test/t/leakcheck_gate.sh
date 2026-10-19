#!/usr/bin/env bash
# The leak gate (scripts/leakcheck.sh) end to end: sift._test_leak leaves each kind of resource behind on purpose in a
# real server, and the gate must fail on every one of them (and on a crashed backend and a session left in a
# transaction) while a clean log passes. The deliberate findings of this node are set aside before finish, which runs the
# gate on the node like on any other and must find it clean. Under SANITIZE=1 the malloc leak must be reported by
# LeakSanitizer, and nothing else: PostgreSQL's own allocations are suppressed (scripts/lsan.supp).
. "$(dirname "$0")/lib.bash"

D=$(dir gate)
asan=
[[ -z ${SIFT_PRELOAD:-} ]] || asan=1
plan 12
# the sanitizer reports of this node stay in its directory, away from the scans of the whole run
[[ -z $asan ]] || export ASAN_OPTIONS=${ASAN_OPTIONS%log_path=*}log_path=$D/asan
node_init gate 1
node_start gate || bail "start failed"
sql gate "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "setup failed"
LOG=$D/postgres.log

# newfindings OFFSET: the log written since OFFSET, as a file the gate scans by name
slice() { tail -c +"$(($1 + 1))" "$LOG" >"$D/slice.out"; }
gate_fails() { slice "$1"; ! leak_scan "$D/slice.out" 2>"$D/gate.out" && grep -qE -e "$2" "$D/gate.out"; }
logsize() { stat -c %s "$LOG"; }
# the cluster checks of this node record their findings next to it
cluster_fails() { local out; ! out=$(PGHOST=$D/sock PGPORT=$(port gate) SIFT_TMP=$D leak_cluster "$D/data" 2>&1) && grep -q "$1" <<<"$out"; }
asan_fails() { ! leak_scan "$D"/asan.* 2>/dev/null; }
asan_only() { local f; for f in "$D"/asan.*; do grep -q sift_test_leak "$f" || return 1; done; }

sql gate "CREATE TABLE t AS SELECT g FROM generate_series(1, 100) g" "SELECT count(*) FROM t" >/dev/null
slice 0
check "the gate passes a clean server log" leak_scan "$D/slice.out"

for spec in "lock:you don.t own a lock" "buffer:resource was not closed" "relcache:resource was not closed" \
	"tupledesc:resource was not closed" "snapshot:resource was not closed"; do
	kind=${spec%%:*}
	off=$(logsize)
	sql gate "BEGIN" "SELECT sift._test_leak('$kind')" "COMMIT" >/dev/null 2>&1
	check "the gate fails on a leaked $kind" gate_fails "$off" "${spec#*:}"
done

# the context and the malloc leaks show at backend exit, after the session ended
off=$(logsize)
sql gate "SELECT sift._test_leak('context')" >/dev/null
for _ in $(seq 100); do grep -q 'sift leak: memory context' "$LOG" && break; sleep 0.1; done
check "the gate fails on a sift memory context that outlives its backend" gate_fails "$off" 'sift leak: memory context "sift leaked"'

sql gate "SELECT sift._test_leak('malloc')" >/dev/null
if [[ -n $asan ]]; then
	for _ in $(seq 100); do grep -qs sift_test_leak "$D"/asan.* && break; sleep 0.1; done
	check "the gate fails on a malloc leak (LeakSanitizer)" asan_fails
else
	skip 1 "malloc leaks need the sanitizer build (SANITIZE=1)"
fi

# a backend whose memory keeps growing, next to an idle one: the soak's growth check tells them apart. The leak is about 300 KB a call: at
# 50 calls a second the 3 s window after the warmup grows far past the 8 MB threshold, even when the host slows the loop several times.
# The sleeps below do not hold the stderr of the test: prove reads it and would wait for them after the last test
PGAPPNAME=leaker psql -X -q -h "$D/sock" -p "$(port gate)" -U postgres -d postgres >/dev/null 2>&1 <<<"SELECT sift._test_leak('malloc')\\watch 0.02" &
leaker=$!
PGAPPNAME=idler psql -X -q -h "$D/sock" -p "$(port gate)" -U postgres -d postgres >/dev/null 2>&1 < <(sleep 30 2>/dev/null) &
idler=$!
app_pids() { sql gate "SELECT pid FROM pg_stat_activity WHERE application_name IN ('leaker', 'idler') ORDER BY application_name"; }
for _ in $(seq 100); do [[ $(app_pids | wc -l) == 2 ]] && break; sleep 0.1; done
leak_watch_start "$D/memory.tsv" app_pids 0.25
sleep 5
LEAK_QUARANTINE_MB=0 leak_watch_stop "$D/memory.tsv" >"$D/growth.out"
kill "$leaker" "$idler" 2>/dev/null
check "the growth check fails the backend that leaks, not the idle one" bash -c \
	'[[ $(grep -c GROWING "$1") == 1 ]] && [[ $(grep -c " ok$" "$1") == 1 ]]' _ "$D/growth.out"

# a session that stays in a transaction is a leaked resource of the test that opened it
( echo "BEGIN; CREATE TABLE held (i int); SELECT 1;"; sleep 30 ) 2>/dev/null |
	psql -X -q -h "$D/sock" -p "$(port gate)" -U postgres -d postgres >/dev/null 2>&1 &
holder=$!
for _ in $(seq 100); do [[ $(sql gate "SELECT count(*) FROM pg_stat_activity WHERE state = 'idle in transaction'") == 1 ]] && break; sleep 0.1; done
check "the cluster check fails on a session left in a transaction" cluster_fails "idle in transaction"
kill "$holder" 2>/dev/null

# a backend that dies on a signal is a crash
off=$(logsize)
sql gate "SELECT pg_sleep(60)" >/dev/null 2>&1 &
sleeper=$!
for _ in $(seq 100); do [[ $(sql gate "SELECT count(*) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(60)%' AND pid <> pg_backend_pid()") == 1 ]] && break; sleep 0.1; done
kill -KILL "$(sql gate "SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(60)%' AND pid <> pg_backend_pid()")"
wait "$sleeper" 2>/dev/null
for _ in $(seq 100); do grep -q 'terminated by signal' "$LOG" && break; sleep 0.1; done
check "the gate fails on a backend killed by a signal" gate_fails "$off" 'terminated by signal 9'

if [[ -n $asan ]]; then
	check "LeakSanitizer reports only the deliberate leak, not PostgreSQL's own allocations" asan_only
else
	skip 1 "LeakSanitizer needs the sanitizer build (SANITIZE=1)"
fi

# set the deliberate findings aside: finish runs the gate on this node and must find nothing else
node_stop gate
mv "$LOG" "$D/postgres.deliberate"
mv "$D/leakcheck.findings" "$D/findings.deliberate"
finish
