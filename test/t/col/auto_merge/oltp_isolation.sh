#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Spec 97 track 4: the auto-merge of a sift_col index, a sift_table and a sift BM25 index beside four OLTP writers and four SELECT readers. Per engine and per size (16384 and 32768 rows)
# the p99 of the writers and of the readers with the merge on and a churn backlog is divided by their p99 with the merge off (R, each p99 the median of three windows), and the stall of the double size (R - 1, at least 0.5, the noise
# of a p99) by the stall of the single one (the growth factor): a lock or a buffer flood that costs the work of the table (O(N)) doubles it, a bounded one does not. R of the writers stays within 2.5, R of the readers within 2.0, the
# growth factor within 1.25. The pacer (sift_internal.maint_pacer) must throttle the worker while they run, give the full speed back when they end, nothing fails, and sift.check finds no problem.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 19
node_init iso 1 "shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
autovacuum = off
shared_buffers = '16MB'
max_worker_processes = 16
max_parallel_workers = 4
maintenance_work_mem = '4MB'
temp_file_limit = '1MB'
sift.max_maint_workers = 2
sift.maintenance_naptime = 200
sift.maintenance_cost_delay = 0
deadlock_timeout = '200ms'
lock_timeout = '3s'            # timeout-ok: a writer or a reader that waits that long for a lock is the stall this test is about
statement_timeout = '5s'       # timeout-ok: a statement that takes that long is the stall this test is about"
node_start iso || bail "start failed"
sql iso "CREATE EXTENSION pg_sift CASCADE" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension setup failed"

D=$(dir iso)
QUIET_S=1 ACTIVE_S=1 RUNS=3 CLIENTS=4
CONN=(-h "$(node_sock iso)" -p "$(port iso)" -U postgres)
DB=$(node_db iso)

# p99_us GLOB: the 99th percentile of the latencies (microseconds, column 3) of the pgbench -l logs
p99_us() { cat $1 | awk '{ print $3 }' | sort -n | awk '{ a[NR] = $1 } END { i = int((NR * 99 + 99) / 100); if (i > NR) i = NR; print a[i] + 0 }'; }
ratio() { awk -v a="$1" -v q="$2" 'BEGIN { printf "%.2f", a / (q < 100 ? 100 : q) }'; }
within() { awk -v v="$1" -v m="$2" 'BEGIN { exit !(v <= m) }'; }

# pair TAG ASYNC SCRIPT_W SCRIPT_R SECONDS: the writers and the readers for SECONDS with sift.async_maintenance ASYNC; their p99 in W_P99 and R_P99, their failures in FAILS
pair() {
	local tag=$1 async=$2 w=$3 r=$4 secs=$5 side
	rm -f "$D/$tag".*
	for side in w r; do
		local script=$w; [[ $side == r ]] && script=$r
		PGOPTIONS="-c sift.async_maintenance=$async -c enable_seqscan=off" pgbench -n "${CONN[@]}" -c $CLIENTS -j $CLIENTS -T "$secs" \
			-l --log-prefix="$D/$tag.$side.log" -f "$script" "$DB" >"$D/$tag.$side.out" 2>&1 &
		eval "PID_$side=$!"
	done
	NAPS=$(naps)
	[[ $async == on ]] && sample_pacer "$tag" "$secs"
	FAILS=0
	for side in w r; do
		eval "wait \$PID_$side" || FAILS=$((FAILS + 1))
		FAILS=$((FAILS + $(sed -n 's/^number of failed transactions: \([0-9]*\).*/\1/p' "$D/$tag.$side.out") + 0))
	done
	NAPS=$(($(naps) - NAPS))
	W_P99=$(p99_us "$D/$tag.w.log.*")
	R_P99=$(p99_us "$D/$tag.r.log.*")
}

# quiet TAG SCRIPT_W SCRIPT_R: the p99 with the merge off, the median of RUNS windows (one window of a second on a fresh table is as often the cold cache or a busy host as the engine)
quiet() {
	local i ws=() rs=() fails=0
	for i in $(seq "$RUNS"); do
		pair "$1.$i" off "$2" "$3" $QUIET_S
		ws+=("$W_P99") rs+=("$R_P99") fails=$((fails + FAILS))
	done
	diag "$1 windows: write p99 ${ws[*]} us, select p99 ${rs[*]} us"
	W_P99=$(median "${ws[@]}") R_P99=$(median "${rs[@]}") FAILS=$fails
}

# active ENG TBL ROWS: the p99 with the merge on, the median of RUNS windows, each over a churn backlog of its own that the worker merges; the pacer samples and the naps of all of them
active() {
	local eng=$1 tbl=$2 i ws=() rs=() fails=0 seen=0 throttled=0 naps=0 delay=0
	for i in $(seq "$RUNS"); do
		churn_$eng "$tbl" "$3" | sql iso >/dev/null || bail "$tbl churn failed"
		enqueue_$eng "$tbl"
		pair "$tbl.active.$i" on "$D/$tbl.w.sql" "$D/$tbl.r.sql" $ACTIVE_S
		ws+=("$W_P99") rs+=("$R_P99") fails=$((fails + FAILS)) seen=$((seen + SEEN)) throttled=$((throttled + THROTTLED)) naps=$((naps + NAPS))
		((MAX_DELAY > delay)) && delay=$MAX_DELAY
		wait_for drained 10
	done
	diag "$tbl active windows: write p99 ${ws[*]} us, select p99 ${rs[*]} us"
	W_P99=$(median "${ws[@]}") R_P99=$(median "${rs[@]}") FAILS=$fails SEEN=$seen THROTTLED=$throttled NAPS=$naps MAX_DELAY=$delay
}
median() { printf '%s\n' "$@" | sort -n | awk '{ a[NR] = $1 } END { print a[int((NR + 1) / 2)] }'; }

naps() { grep -c 'sift test: throttle nap' "$D/postgres.log"; }

# sample_pacer TAG SECONDS: the pacer every 100 ms of the first SECONDS-0.3 s, in $D/TAG.pacer, counted into SEEN (workers running) and THROTTLED (a mode that sleeps); NAPS counts the naps the worker took
sample_pacer() {
	psql -X -At -h "$(node_sock iso)" -p "$(port iso)" -U postgres -d "$DB" >"$D/$1.pacer" 2>&1 <<-SQL
		SELECT workers_active || '|' || pacer_mode || '|' || pacer_delay_ms FROM sift_internal.maint_pacer() \watch i=0.1 c=$(($2 * 10 - 3))
	SQL
	SEEN=$(awk -F'|' '$1 > 0' "$D/$1.pacer" | wc -l)
	THROTTLED=$(awk -F'|' '$2 ~ /^(moderate|throttled|deferred)$/ && $3 > 0' "$D/$1.pacer" | wc -l)
	MAX_DELAY=$(awk -F'|' '$3 > m { m = $3 } END { print m + 0 }' "$D/$1.pacer")
}

drained() { [[ $(sql iso "SELECT workers_active FROM sift_internal.maint_pacer()") -eq 0 ]]; }
enqueue() { # RELATION KIND...: the maintenance requests a trigger would make, with the merge on
	local rel=$1 kind got
	shift
	for kind in "$@"; do
		got=$(sql iso "SET sift.async_maintenance = on" "SELECT sift._test_maint_enqueue('$rel'::regclass, $kind)" | tail -1)
		[[ $got == queued || $got == coalesced ]] || bail "the request $kind for $rel was $got"
	done
}

# the tables, the scripts of the writers and of the readers, the backlog of a churn, and the requests, of each engine
setup_col_f2() {
	sql iso "CREATE TABLE $1 (id int, grp int, ts timestamptz, payload text)" \
		"INSERT INTO $1 SELECT g, g % 64, timestamptz '2026-01-01' + (g || ' seconds')::interval, repeat(md5(g::text), 4) FROM generate_series(1, $2) g" \
		"CREATE INDEX $1_idx ON $1 USING sift_col (id, grp, ts, payload)"
	printf '%s\n' "\\set id random(1, $2)" "UPDATE $1 SET grp = (grp + 1) % 64 WHERE id = :id;" >"$D/$1.w.sql"
	printf '%s\n' "\\set g random(0, 63)" "SELECT count(*), sum(id) FROM $1 WHERE grp = :g;" >"$D/$1.r.sql"
}
churn_col_f2() {
	local r
	for r in 1 2 3 4 5 6 7 8; do
		echo "DELETE FROM $1 WHERE id % 32 = $r;"
		echo "INSERT INTO $1 SELECT g, g % 64, timestamptz '2026-01-01' + (g || ' seconds')::interval, repeat(md5(g::text), 4) FROM generate_series($(($2 + r * 512 + 1)), $(($2 + r * 512 + 512))) g;"
	done
}
enqueue_col_f2() { enqueue "$1_idx" 1 2 3 8; }
check_col_f2() { echo "$1_idx"; }

setup_tbl_f2() {
	sql iso "CREATE TABLE $1 (id int PRIMARY KEY, grp int, val int, ts timestamptz, payload text) USING sift_table" \
		"INSERT INTO $1 SELECT g, g % 64, g % 1000, timestamptz '2026-01-01' + (g || ' seconds')::interval, repeat(md5(g::text), 4) FROM generate_series(1, $2) g"
	printf '%s\n' "\\set id random(1, $2)" "UPDATE $1 SET val = val + 1 WHERE id = :id;" >"$D/$1.w.sql"
	printf '%s\n' "\\set id random(1, $2)" "SELECT id, val, grp FROM $1 WHERE id = :id;" >"$D/$1.r.sql"
}
churn_tbl_f2() {
	local r
	for r in 1 2 3 4 5 6 7 8; do
		echo "UPDATE $1 SET val = val + $r WHERE id % 16 = $r;"
		echo "DELETE FROM $1 WHERE id % 64 = $r;"
	done
}
enqueue_tbl_f2() { enqueue "$1" 1 2 3 8; }
check_tbl_f2() { echo "$1"; }

setup_bm25() {
	sql iso "SET temp_file_limit = -1" "CREATE TABLE $1 (id serial PRIMARY KEY, body text)" \
		"INSERT INTO $1 (body) SELECT 'common ' || md5(g::text) || ' tok' || (g % 200) || ' ' || repeat('alpha ', 8) FROM generate_series(1, $2) g" \
		"CREATE INDEX $1_idx ON $1 USING sift (body) WITH (write_buffer_limit = 128)"
	printf '%s\n' "INSERT INTO $1 (body) VALUES ('common tok' || (random() * 200)::int || ' ' || md5(random()::text) || ' beta gamma delta');" >"$D/$1.w.sql"
	printf '%s\n' "\\set t random(0, 199)" "SELECT id FROM $1 WHERE body @@> ('tok' || :t) LIMIT 20;" >"$D/$1.r.sql"
}
churn_bm25() {
	local r
	for r in 1 2 3 4 5 6 7 8; do
		echo "DELETE FROM $1 WHERE id % 32 = $r;"
		echo "INSERT INTO $1 (body) SELECT 'common ' || md5((g + $r)::text) || ' tok' || (g % 200) || ' ' || repeat('alpha ', 8) FROM generate_series(1, 512) g;"
	done
}
enqueue_bm25() { enqueue "$1_idx" 5; }
check_bm25() { echo "$1_idx"; }

# churn_row CASE R1 R2 MAX: the line of the table of the spec and its verdict as a test. The stall of a size is what its R adds to the quiet p99 (R - 1), never under the 0.5 of the noise of a p99
churn_row() {
	local name=$1 r1=$2 r2=$3 max=$4 gf
	gf=$(awk -v a="$r1" -v b="$r2" 'BEGIN { a -= 1; b -= 1; if (a < 0.5) a = 0.5; if (b < 0.5) b = 0.5; printf "%.2f", b / a }')
	local verdict=FAIL
	{ within "$r1" "$max" && within "$r2" "$max" && within "$gf" 1.25; } && verdict=PASS
	diag "CHURN_ROW $name | ${r1}x | ${r2}x | n/a | $gf | R<=${max}x,growth<=1.25 | $verdict"
	[[ $verdict == PASS ]]
}

run_engine() {
	local eng=$1 scale rows tbl seen=0 throttled=0 slept=0 fails=0 rel
	local -A rw rs
	for scale in 1 2; do
		rows=$((16384 * scale))
		tbl=${eng}_${scale}x
		setup_$eng "$tbl" "$rows" >/dev/null || bail "$tbl setup failed"
		quiet "$tbl.quiet" "$D/$tbl.w.sql" "$D/$tbl.r.sql"
		local qw=$W_P99 qr=$R_P99
		fails=$((fails + FAILS))
		active "$eng" "$tbl" "$rows"
		fails=$((fails + FAILS)) seen=$((seen + SEEN)) throttled=$((throttled + THROTTLED)) slept=$((slept + NAPS))
		rw[$scale]=$(ratio "$W_P99" "$qw") rs[$scale]=$(ratio "$R_P99" "$qr")
		diag "$eng ${scale}x: write p99 ${qw}us -> ${W_P99}us (R ${rw[$scale]}), select p99 ${qr}us -> ${R_P99}us (R ${rs[$scale]}), worker samples $SEEN, throttled samples $THROTTLED, worker naps $NAPS, longest pacer delay ${MAX_DELAY}ms"
		rel=$(check_$eng "$tbl")
		check "$eng ${scale}x: sift.check($rel, true) finds no problem" test "$(sql iso "SELECT count(*) FROM sift.check('$rel', true)")" -eq 0
	done
	check "$eng: write p99 within 2.5x of the quiet one and its growth from 1x to 2x within 1.25" churn_row "${eng}_write_p99" "${rw[1]}" "${rw[2]}" 2.5
	check "$eng: select p99 within 2.0x of the quiet one and its growth from 1x to 2x within 1.25" churn_row "${eng}_select_p99" "${rs[1]}" "${rs[2]}" 2.0
	check "$eng: the worker is throttled while the writers and readers run (worker samples $seen, throttled samples $throttled, naps $slept)" \
		test "$seen" -gt 0 -a "$throttled" -gt 0 -a "$slept" -gt 0
	check "$eng: no transaction failed, deadlocked or timed out" test "$fails" -eq 0 -a "$(grep -c 'deadlock detected' "$D/postgres.log")" -eq 0
}

run_engine col_f2
run_engine tbl_f2
run_engine bm25

recovered() { [[ $(sql iso "SELECT pacer_mode || ':' || pacer_delay_ms FROM sift_internal.maint_pacer()") == full:0 ]]; }
wait_for recovered 2
check "the pacer is back to full:0 when the writers and readers end (it is $(sql iso "SELECT pacer_mode || ':' || pacer_delay_ms FROM sift_internal.maint_pacer()"))" recovered
finish
