#!/usr/bin/env bash
# Resource and memory leak gate shared by every suite. Nothing in a suite refers to it by test: the hooks sit on the
# primitives all of them use (pgtest.sh conf/start/stop, test/t/lib.bash nodes, scripts/tap.sh, scripts/ci.sh), so a new
# test is covered without editing it.
#
#   scripts/leakcheck.sh scan PATH...        fail on leak diagnostics in the server logs and sanitizer reports under PATH
#                                            (files as given, directories searched for postgres.log, tap/log/*.log, sanitizer/*)
#   scripts/leakcheck.sh cluster DATA        invariants of a running cluster: no session left holding locks or a transaction, no
#                                            prepared transaction, long-lived processes (postmaster, workers) not grown
#   scripts/leakcheck.sh stopped DATA        after the shutdown: no temporary file left behind
#   scripts/leakcheck.sh baseline DATA       records the memory of those processes (the start of the cluster)
#   scripts/leakcheck.sh growth FILE         sustained memory growth in the samples of leak_watch
#   scripts/leakcheck.sh conf | env DIR      postgresql.conf lines | exports that run a server under ASan/UBSan/LeakSanitizer
#   scripts/leakcheck.sh expect COUNT REGEX  a test that crashes or leaks on purpose declares the findings it expects
#   scripts/leakcheck.sh selftest            proves the gate fails on every kind of leak and passes on a clean log
#
# Sourced (. scripts/leakcheck.sh), it defines the leak_* functions. Server side, PostgreSQL reports what a transaction or
# backend leaves behind (buffer pins, relcache, TupleDesc, snapshot, lock and file references, "you don't own a lock",
# assertion failures, crashes) as log lines, and an ASan/UBSan/LeakSanitizer build reports memory errors and malloc leaks
# when each server process exits (LeakSanitizer needs the clean exit of a fast shutdown, an immediate one reports nothing).
# SIFT_TESTING builds also log the sift memory contexts that outlive a backend (src/core/sift_leak_test.c). Exit 1 on a finding.
#
# LEAK_PROC_MB (32): growth of a long-lived server process; LEAK_GROWTH_MB (8), LEAK_GROWTH_R2 (0.8): growth of a sampled
# backend over the post-warmup samples that fails the soak (plus LEAK_QUARANTINE_MB on an ASan server).
LEAK_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# regex <TAB> a sample log line it must match: one row per kind of leak. The selftest feeds every sample to the scanner.
LEAK_RULES=(
	$'resource was not closed\tWARNING:  resource was not closed: relation "pg_class"'
	$'buffer refcount leak\tWARNING:  buffer refcount leak: [123] (rel=base/5/1259, blockNum=0, flags=0x93800000, refcount=1 1)'
	$'reference leak\tWARNING:  TupleDesc reference leak: TupleDesc 0x55d0 (1259,-1) still referenced'
	$'is not owned by resource owner\tERROR:  relcache reference 0x7f12 is not owned by resource owner TopTransaction'
	$'snapshot [^ ]* still active\tWARNING:  snapshot 0x55d0c8 still active'
	$'you don\'t own a lock\tWARNING:  you don\'t own a lock of type ExclusiveLock'
	$'lock [^ ]* still held\tWARNING:  lock ExclusiveLock still held at transaction end'
	$'temporary files? (leak|and directories not closed)\tWARNING:  1 temporary files and directories not closed at end-of-transaction'
	$'leaking still-referenced relcache entry\tWARNING:  leaking still-referenced relcache entry for "pg_class"'
	$'is still referenced \\(local\tERROR:  block 3 of base/5/16384 is still referenced (local 1)'
	$'leaked (hash_seq_search|parallel context|AIO handle)\tWARNING:  leaked hash_seq_search scan for hash table 0x55d0'
	$'TRAP:\tTRAP: failed Assert("x == 1"), File: "sift_eval.c", Line: 1, PID: 42'
	$'PANIC:\tPANIC:  could not locate a valid checkpoint record'
	$'terminated by (signal|exception)\tLOG:  server process (PID 42) was terminated by signal 11: Segmentation fault'
	$'ERROR: (Address|Leak)Sanitizer\t==42==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x1'
	$'runtime error:\tsrc/sift_bitmap.c:10:5: runtime error: shift exponent 64 is too large'
	$'SUMMARY: (Address|Undefined|Leak)Sanitizer\tSUMMARY: LeakSanitizer: 1000 byte(s) leaked in 1 allocation(s).'
	$'leaked server state\tleaked server state: session 42 idle holds ExclusiveLock on t'
	$'sift leak: memory context\tLOG:  sift leak: memory context "sift scan" (8192 bytes) still allocated in "TopMemoryContext" at backend exit'
)

leak_regex() { printf '%s\n' "${LEAK_RULES[@]}" | cut -f1 | paste -sd'|'; }

# postgresql.conf lines every test server gets (through pgtest.sh conf): the leak warnings are WARNING level and carry the pid
leak_conf() {
	echo "log_min_messages = warning"
	echo "log_line_prefix = '%m [%p] '"
}

# leak_sanitizer_env DIR: runs the servers started from this shell under the sanitizers; reports go to DIR/asan.<pid>.
# LeakSanitizer reports at every process exit and must not turn it into a crash (the postmaster restarts all
# on an unexpected exit code), so exitcode=0; scripts/lsan.supp lists PostgreSQL's own startup allocations.
leak_sanitizer_env() {
	mkdir -p "$1"
	export LD_PRELOAD=$(gcc -print-file-name=libasan.so):$(gcc -print-file-name=libubsan.so)
	export ASAN_OPTIONS=detect_leaks=1:exitcode=0:abort_on_error=1:log_path=$1/asan
	export LSAN_OPTIONS=suppressions=$LEAK_DIR/lsan.supp:print_suppressions=0
	export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:log_path=$1/ubsan
}

# ---------- log scanning ----------

# leak_logs PATH...: the files to scan, files as given and directories searched for server logs and sanitizer reports
leak_logs() {
	local p
	for p in "$@"; do
		if [[ -f $p ]]; then echo "$p"
		else find "$p" -type f \( -name postgres.log -o -path '*/tap/log/*.log' -o -path '*/sanitizer/*' -o -name leakcheck.findings \) 2>/dev/null; fi
	done
}

# leak_expect COUNT REGEX: the test about to run crashes or leaks COUNT times on purpose; scans find the file by name
leak_expect() { printf '%s\t%s\n' "$1" "$2" >>"${SIFT_TMP:-/tmp/sift-arch}/leakcheck.expect"; }

# leak_scan PATH...: prints the findings, minus the expected ones, and fails if any remain
leak_scan() {
	local files found
	mapfile -t files < <(leak_logs "$@")
	(( ${#files[@]} )) || return 0
	found=$(grep -aHnE -e "$(leak_regex)" "${files[@]}" 2>/dev/null | leak_unexpected "$@")
	[[ -z $found ]] && return 0
	echo "leakcheck: $(wc -l <<<"$found") finding(s):" >&2
	head -30 <<<"$found" | cut -c1-300 >&2
	return 1
}

# stdin: file:line:text findings; drops the ones the expectation files under PATH... still allow. The expectations are the
# first awk input, after a line no finding can use up, so that having none does not read the findings as expectations.
leak_unexpected() {
	awk -F'\t' 'FNR == NR { n = FNR; cnt[n] = $1; re[n] = $2; next }
		{ for (i = 1; i <= n; i++) if (cnt[i] > 0 && $0 ~ re[i]) { cnt[i]--; next } print }' \
		<(echo -e '0\tnothing'; for p in "$@"; do [[ -d $p ]] && find "$p" -name leakcheck.expect -exec cat {} +; done 2>/dev/null) -
}

# ---------- cluster invariants ----------

leak_psql() { psql -X -qAt -v ON_ERROR_STOP=1 -U postgres -d postgres "$@"; }

# leak_procs DATA: "pid starttime kB command" of the processes that live as long as the cluster: the postmaster and its
# auxiliary processes, found by their process titles
leak_procs() {
	local pid cmd
	for pid in $(head -1 "$1/postmaster.pid") $(pgrep -P "$(head -1 "$1/postmaster.pid")"); do
		cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null) || continue
		[[ $pid == $(head -1 "$1/postmaster.pid") || $cmd =~ ^postgres:\ (checkpointer|background\ writer|walwriter|autovacuum\ launcher|logical\ replication\ launcher|startup|walreceiver|archiver|io\ worker) ]] ||
			continue
		echo "$pid $(cut -d' ' -f22 "/proc/$pid/stat") $(leak_anon_kb "$pid") ${cmd:0:40}"
	done
}

# Private anonymous memory of a process in kB: palloc and malloc, not the shared buffers
leak_anon_kb() { awk '/^Anonymous:/ { print $2 }' "/proc/$1/smaps_rollup" 2>/dev/null; }

leak_baseline() { leak_procs "$1" >"$1/leakcheck.baseline"; }

# leak_finding TEXT...: a finding of the cluster checks, kept in $SIFT_TMP for the scans of the whole run
leak_finding() { echo "leaked server state: $*" | tee -a "${SIFT_TMP:-/tmp/sift-arch}/leakcheck.findings" >&2; }

# leak_cluster DATA: run against the cluster in DATA before it stops (PGHOST/PGPORT select it)
leak_cluster() {
	local data=$1 bad=0 line pid start kb name now
	while read -r line; do leak_finding "$line"; bad=1; done < <(leak_psql <<-'SQL' 2>&1 || echo "cannot query the cluster"
		SELECT format('session %s %s holds %s on %s', a.pid, a.state, l.mode, coalesce(l.relation::regclass::text, l.locktype))
		FROM pg_stat_activity a JOIN pg_locks l USING (pid)
		WHERE a.backend_type = 'client backend' AND a.pid <> pg_backend_pid() AND l.granted
			AND l.locktype NOT IN ('virtualxid', 'transactionid') AND a.state = 'idle';
		SELECT format('session %s is %s since %s', pid, state, xact_start) FROM pg_stat_activity
		WHERE backend_type = 'client backend' AND pid <> pg_backend_pid() AND state LIKE 'idle in transaction%';
		SELECT 'prepared transaction ' || gid FROM pg_prepared_xacts;
	SQL
	)
	while read -r pid start kb name; do
		[[ -r /proc/$pid/stat && $(cut -d' ' -f22 "/proc/$pid/stat") == "$start" ]] || continue
		now=$(leak_anon_kb "$pid")
		(( now - kb <= ${LEAK_PROC_MB:-32} * 1024 )) || { leak_finding "$name (pid $pid) grew $(( (now - kb) / 1024 )) MB since the start"; bad=1; }
	done <"$data/leakcheck.baseline" 2>/dev/null
	return $bad
}

# leak_after_stop DATA: a clean shutdown ends every backend, and each one removes its temporary files
leak_after_stop() {
	local f bad=0
	for f in $(find "$1" -path '*/pgsql_tmp/*' -type f 2>/dev/null); do leak_finding "temporary file left after shutdown: $f"; bad=1; done
	return $bad
}

# ---------- memory growth ----------

# leak_sample FILE PID...: appends "epoch pid kB" per process
leak_sample() {
	local out=$1 pid
	shift
	for pid in "$@"; do echo "$EPOCHSECONDS $pid $(leak_anon_kb "$pid")"; done >>"$out"
}

# leak_watch_start FILE PIDS_CMD [SECONDS]: samples the processes PIDS_CMD prints every SECONDS (2) in the background
leak_watch_start() {
	( while :; do leak_sample "$1" $($2); sleep "${3:-2}"; done ) &
	LEAK_WATCH=$!
}

# leak_watch_stop FILE: stops the sampler and reports sustained growth
leak_watch_stop() {
	kill "$LEAK_WATCH" 2>/dev/null
	wait "$LEAK_WATCH" 2>/dev/null
	leak_growth "$1"
}

# leak_growth FILE: per process, least squares over the samples after the first third (the warmup of caches): a process
# that grows by LEAK_GROWTH_MB or more over that window along a straight line (R2 >= LEAK_GROWTH_R2) is leaking. An ASan
# server first fills its quarantine of freed memory (LEAK_QUARANTINE_MB, 256, the ASan default), which is no leak either.
leak_growth() {
	local slack=${LEAK_QUARANTINE_MB:-$([[ $(cat "${SIFT_ROOT:-/nonexistent}/.flavor" 2>/dev/null) == asan* ]] && echo 256 || echo 0)}
	sort -k2,2n -k1,1n "$1" | awk -v mb="$(( ${LEAK_GROWTH_MB:-8} + slack ))" -v r2min="${LEAK_GROWTH_R2:-0.8}" '
		{ n[$2]++; t[$2, n[$2]] = $1; v[$2, n[$2]] = $3 }
		END {
			for (p in n) {
				from = int(n[p] / 3) + 1; m = n[p] - from + 1
				if (m < 5) continue
				sx = sy = sxx = sxy = syy = 0
				for (i = from; i <= n[p]; i++) { x = t[p, i] - t[p, from]; y = v[p, i]; sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y }
				den = m * sxx - sx * sx; vy = m * syy - sy * sy
				slope = den ? (m * sxy - sx * sy) / den : 0
				r2 = (den && vy) ? (m * sxy - sx * sy) ^ 2 / (den * vy) : 0
				grow = slope * (t[p, n[p]] - t[p, from]) / 1024
				verdict = (grow >= mb && r2 >= r2min) ? "GROWING" : "ok"
				printf "pid %s: %d samples, %.1f -> %.1f MB, %+.2f MB over %d s (R2 %.2f) %s\n", p, n[p], v[p, from] / 1024, v[p, n[p]] / 1024, grow, t[p, n[p]] - t[p, from], r2, verdict
				if (verdict == "GROWING") bad = 1
			}
			exit bad
		}'
}

# ---------- gate self test ----------

leak_selftest() {
	local d rc=0 rule re sample
	d=$(mktemp -d)
	printf '%s\n' '2026-10-16 12:00:00.000 UTC [1] LOG:  database system is ready to accept connections' \
		'2026-10-16 12:00:01.000 UTC [1] LOG:  background worker "logical replication launcher" (PID 7) exited with exit code 1' \
		'2026-10-16 12:00:02.000 UTC [9] ERROR:  duplicate key value violates unique constraint "t_pkey"' >"$d/postgres.log"
	leak_scan "$d" 2>/dev/null || { echo "not ok: clean log rejected"; rc=1; }
	for rule in "${LEAK_RULES[@]}"; do
		re=${rule%%$'\t'*} sample=${rule#*$'\t'}
		printf '%s\n' "$sample" >"$d/postgres.log"
		if leak_scan "$d" 2>/dev/null; then echo "not ok: missed '$re': $sample"; rc=1; fi
		if ! grep -qE -e "$re" <<<"$sample"; then echo "not ok: sample does not match '$re'"; rc=1; fi
	done
	sample='LOG:  server process (PID 42) was terminated by signal 6: Aborted'
	printf '%s\n' "$sample" "$sample" >"$d/postgres.log"
	SIFT_TMP=$d leak_expect 1 'terminated by signal 6'
	leak_scan "$d" 2>/dev/null && { echo "not ok: second hit allowed by one expectation"; rc=1; }
	SIFT_TMP=$d leak_expect 1 'terminated by signal 6'
	leak_scan "$d" 2>/dev/null || { echo "not ok: expected findings rejected"; rc=1; }
	awk 'BEGIN { for (i = 0; i < 30; i++) print i * 2, 7, 100000 + i * 6000; for (i = 0; i < 30; i++) print i * 2, 8, 100000 + (i % 3) * 50 }' >"$d/mem"
	LEAK_QUARANTINE_MB=0 leak_growth "$d/mem" >"$d/growth" && { echo "not ok: steady growth accepted"; rc=1; }
	grep -q 'pid 7: .*GROWING' "$d/growth" && grep -q 'pid 8: .*ok' "$d/growth" || { echo "not ok: growth verdict per process"; rc=1; }
	rm -rf "$d"
	(( rc )) || echo "ok: ${#LEAK_RULES[@]} leak rules, expectations and growth fit behave"
	return $rc
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
	set -uo pipefail
	cmd=${1:-}
	shift || true
	case $cmd in
		scan) leak_scan "$@" ;;
		cluster) leak_cluster "$@" ;;
		stopped) leak_after_stop "$@" ;;
		baseline) leak_baseline "$@" ;;
		growth) leak_growth "$@" ;;
		conf) leak_conf ;;
		env) leak_sanitizer_env "$1" && env | grep -E '^(LD_PRELOAD|ASAN_OPTIONS|LSAN_OPTIONS|UBSAN_OPTIONS)=' | sed 's/^/export /' ;;
		expect) leak_expect "$@" ;;
		selftest) leak_selftest ;;
		*) sed -n '2,21p' "$0"; exit 1 ;;
	esac
fi
