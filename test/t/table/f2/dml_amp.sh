#!/usr/bin/env bash
# What UPDATE and DELETE churn costs a Format 2.0 sift_table, a sift_col index and a BM25 index, as growth factors: the same churn of a quarter of the rows is run for 4 cycles and the
# 4th may cost no more than a bounded multiple of the 1st in the size of the file, the WAL and the CPU time of the statements; the memory of a fresh backend is read at 16,384, 32,768 and
# 65,536 rows. One CHURN_ROW line per figure ("case:metric | 1x | 2x | 4x | growth_factor | limit | verdict"; the columns are the cycles 1, 2 and 4, or the scales, or the
# transactions 10, 25 and 50 of the hot-key case, or the windows of 300, 300 and 100 of 800 one-row updates made while a repeatable read snapshot is open), then the file is settled
# (VACUUM and the maintenance step) and must be within 1.35 of its clean size and verify with the heap compared. A server of its own: wal_level = replica keeps the
# standby-safe delay of the extent reuse in play.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"
[[ -r /proc/self/status && -r /proc/self/sched ]] || { echo "1..0 # SKIP needs /proc/self/status and /proc/self/sched (Linux)"; exit 0; }

done_testing
node_init ch 71 "wal_level = replica
autovacuum = off
sift.async_maintenance = off
sift.standby_conflicts = on
sift.backend_cache_kb = 2048
maintenance_work_mem = '4MB'
temp_file_limit = '1MB'"
node_start ch || bail "start failed"
sql ch "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

CASE_T0=$SECONDS

# ratio A B: A / B with two decimals
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", (b > 0 ? a / b : 0) }'; }
# row CASE:METRIC V1 V2 V4 GROWTH LIMIT: one CHURN_ROW line, as a comment for the log and as the result
row() {
	local v=FAIL line
	awk -v g="$5" -v l="$6" 'BEGIN { exit !(g <= l) }' && v=PASS
	line="CHURN_ROW $1 | $2 | $3 | $4 | $5 | <= $6 | $v"
	diag "$line"
	if [[ $v == PASS ]]; then ok "$line"; else not_ok "$line"; fi
}
# ratio_row CASE:METRIC V1 V2 V4 LIMIT: the growth is the 4x over the 1x
ratio_row() { row "$1" "$2" "$3" "$4" "$(ratio "$4" "$2")" "$5"; }
# done_case NAME: the seconds the case took, for the budget of the script
done_case() { diag "$1: $((SECONDS - CASE_T0)) s"; CASE_T0=$SECONDS; }
# mean N...: the mean of the numbers
mean() { awk '{ s += $1 } END { printf "%.3f", s / NR }' < <(printf '%s\n' "$@"); }

# ANON_KB: the private memory the backend has used at most, in kB: the peak of the process less the shared buffers and the mapped files it has touched
ANON_KB="(SELECT (regexp_match(s, 'VmHWM:\s+(\d+)'))[1]::int - (regexp_match(s, 'RssShmem:\s+(\d+)'))[1]::int - (regexp_match(s, 'RssFile:\s+(\d+)'))[1]::int
	 FROM (SELECT pg_read_file('/proc/self/status') AS s) p)"
# CPU_MS: the milliseconds of CPU the backend has run: the time of a statement that the load of the host does not change
CPU_MS="(SELECT (regexp_match(pg_read_file('/proc/self/sched'), 'sum_exec_runtime\s*:\s*([0-9.]+)'))[1]::numeric)"
temp_bytes() { sql ch "SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()"; }
# problems REL: the rows of sift.check with the heap compared, or 999 when it raises
problems() {
	local out
	out=$(sql ch "SELECT count(*) FROM sift.check('$1', true)" 2>&1) || { diag "sift.check $1: $out"; echo 999; return; }
	echo "$out"
}
# footprint REL: "relation_bytes used_blocks"
footprint() { sql ch "SELECT pg_relation_size('$1') || ' ' || used_blocks FROM sift.space('$1')"; }
# settle REL INDEX STEP: the VACUUM and the maintenance step (sift.maintain unless STEP says another) of an idle relation; sets SETTLE_ERRORS (the errors and warnings they
# raised), USED_VACUUM and USED_POST (the blocks in use after each) and SIZE_POST (the bytes of the file)
settle() {
	local out
	out=$(sql ch "VACUUM $1" 2>&1) || true
	USED_VACUUM=$(footprint "$2")
	USED_VACUUM=${USED_VACUUM#* }
	out+=$'\n'$(sql ch "SELECT ${3:-sift.maintain('$2')}" 2>&1) || true
	SETTLE_ERRORS=$(grep -cE '^(ERROR|WARNING)' <<<"$out") || true
	[[ $SETTLE_ERRORS == 0 ]] || diag "the VACUUM and the maintenance step of $2:"$'\n'"$(grep -E '^(ERROR|WARNING)' <<<"$out" | sort | uniq -c | cut -c1-200)"
	USED_POST=$(footprint "$2")
	SIZE_POST=${USED_POST% *}
	USED_POST=${USED_POST#* }
}
# settled_rows CASE REL CLEAN TB0 [INDEX [STEP [TEMP_MAX]]]: the rows of a case whose cycles are run: the space amplification against the clean file CLEAN ("relation_bytes
# used_blocks"), the errors of the settling, the temporary files the case wrote and the verify
settled_rows() {
	local case=$1 rel=$2 clean=$3 tb0=$4 idx=${5:-$2} temp bad
	settle "$rel" "$idx" "${6:-}"
	temp=$(($(temp_bytes) - tb0))
	bad=$(problems "$idx")
	row "$case:sa_post" "${clean% *}" - "$SIZE_POST" "$(ratio "$SIZE_POST" "${clean% *}")" 1.35
	row "$case:settle_errors" 0 - "$SETTLE_ERRORS" "$SETTLE_ERRORS" 0
	row "$case:temp_bytes" 0 - "$temp" "$temp" "${7:-0}"
	row "$case:verify" 0 - "$bad" "$bad" 0
}
# blocks_rows CASE CLEAN: the blocks in use beyond those of the clean file after the VACUUM and after the maintenance step, which hold the pages of the delta chains the settling leaves
blocks_rows() {
	row "$1:extra_blocks_vacuum" "${2#* }" - "$USED_VACUUM" "$((USED_VACUUM - ${2#* }))" 4
	row "$1:extra_blocks_post" "${2#* }" - "$USED_POST" "$((USED_POST - ${2#* }))" 4
}

# cycle REL POST SQL...: "wal_bytes ms relation_bytes" of one cycle: the WAL of the statements and of the POST statement that follows them, the CPU milliseconds of the statements only. A checkpoint
# comes first so that every cycle pays the same first-touch page images
cycle() {
	local rel=$1 post=$2
	shift 2
	{
		echo "CHECKPOINT;"
		echo "SELECT pg_current_wal_lsn() AS l0, $CPU_MS AS c0 \\gset"
		printf '%s;\n' "$@"
		echo "SELECT round($CPU_MS - :c0, 1) AS ms \\gset"
		[[ -z $post ]] || echo "$post;"
		echo "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), :'l0')::int8 || ' ' || :ms || ' ' || pg_relation_size('$rel')"
	} | sql ch | tail -1
}
# churn_cycles CASE REL POST: the four cycles of CYCLE_SQL c (one statement a line), "wal ms size" of each in $CYC1 to $CYC4
churn_cycles() {
	local c out stmts
	for c in 1 2 3 4; do
		mapfile -t stmts < <(CYCLE_SQL "$c")
		out=$(cycle "$2" "$3" "${stmts[@]}") || bail "$1 cycle $c failed"
		diag "$1 cycle $c: wal bytes, ms, relation bytes: $out"
		printf -v "CYC$c" %s "$out"
	done
}
# cycle_rows CASE: the growth of the file, the WAL and the time over the cycles of churn_cycles
cycle_rows() {
	local w1 w2 w4 m1 m2 m4 s1 s2 s4
	read -r w1 m1 s1 <<<"$CYC1"
	read -r w2 m2 s2 <<<"$CYC2"
	read -r w4 m4 s4 <<<"$CYC4"
	ratio_row "$1:alpha_disk_cycles" "$s1" "$s2" "$s4" 1.60
	ratio_row "$1:alpha_wal" "$w1" "$w2" "$w4" 1.25
	ratio_row "$1:alpha_dml_time" "$m1" "$m2" "$m4" 1.30
}
# mem_rows CASE LOAD SQL...: the private peak of a fresh backend that runs the statements (TBL is the table) on 16,384, 32,768 and 65,536 rows: its growth and the bytes it adds for a row
mem_rows() {
	local case=$1 load=$2 n peaks=()
	shift 2
	for n in 16384 32768 65536; do
		"$load" "m$n" "$n"
		peaks+=("$(printf '%s;\n' "${@//TBL/m$n}" "SELECT $ANON_KB" | sql ch | tail -1)")
		sql ch "DROP TABLE m$n" >/dev/null
	done
	diag "$case: private peak kB at 16,384 / 32,768 / 65,536 rows: ${peaks[*]}"
	ratio_row "$case:alpha_mem" "${peaks[@]}" 1.15
	row "$case:beta_rss" "${peaks[@]}" "$(awk -v a="${peaks[0]}" -v b="${peaks[2]}" 'BEGIN { printf "%.1f", (b - a) * 1024 / (65536 - 16384) }')" 8.0
}

# load_narrow TABLE ROWS: a sift_table whose columns are all unindexed but the key
load_narrow() {
	sql ch "CREATE TABLE $1 (id int PRIMARY KEY, grp int NOT NULL, v1 bigint NOT NULL, v2 bigint NOT NULL, v3 int NOT NULL, pad text NOT NULL) USING sift_table" \
		"INSERT INTO $1 SELECT g, g % 64, g, g * 3, g % 1000, repeat('x', 40) FROM generate_series(1, $2) g" "VACUUM $1" "SELECT sift.maintain('$1')" >/dev/null || bail "load $1 failed"
}
# load_wide TABLE ROWS: the same with an index on grp: the update of grp is no narrow change
load_wide() {
	load_narrow "$1" "$2"
	sql ch "CREATE INDEX ${1}_grp ON $1 (grp)" "VACUUM $1" "SELECT sift.maintain('$1')" >/dev/null || bail "index of $1 failed"
}

# 1. tbl_f2_narrow_cd: one non-key column of a quarter of the rows, four times, and the memory of that on three sizes
CYCLE_SQL() { echo "UPDATE nar SET v1 = v1 + 1 WHERE id % 4 = $(($1 - 1))"; }
N=32768
load_narrow nar $N
clean=$(footprint nar)
tb0=$(temp_bytes)
churn_cycles tbl_f2_narrow_cd nar ""
cycle_rows tbl_f2_narrow_cd
mem_rows tbl_f2_narrow_cd load_narrow "UPDATE TBL SET v1 = v1 + 1 WHERE id % 4 = 0" "UPDATE TBL SET v1 = v1 + 1 WHERE id % 4 = 1"
settled_rows tbl_f2_narrow_cd nar "$clean" "$tb0"
blocks_rows tbl_f2_narrow_cd "$clean"
done_case tbl_f2_narrow_cd

# 2. tbl_f2_hotkey_zipf: the hot 1% of the rows updated in 50 transactions, one by one in one session: the time of the transactions 1-10, 16-25 and 41-50, the memory at 10, 25 and 50
load_narrow hot 32768
clean=$(footprint hot)
tb0=$(temp_bytes)
out=$({
	for k in $(seq 50); do
		echo "SELECT $CPU_MS AS c0 \\gset"
		echo "UPDATE hot SET v1 = v1 + 1 WHERE id % 100 = 0;"
		echo "SELECT 'T $k ' || round($CPU_MS - :c0, 3);"
		(( k != 10 && k != 25 && k != 50 )) || echo "SELECT 'M $k ' || $ANON_KB;"
	done
} | sql ch) || bail "hot key transactions failed"
t() { awk -v lo="$1" -v hi="$2" '$1 == "T" && $2 >= lo && $2 <= hi { print $3 }' <<<"$out"; }
m() { awk -v k="$1" '$1 == "M" && $2 == k { print $3 }' <<<"$out"; }
t1=$(mean $(t 1 10)) t2=$(mean $(t 16 25)) t4=$(mean $(t 41 50))
ratio_row tbl_f2_hotkey_zipf:alpha_dml_time "$t1" "$t2" "$t4" 1.30
ratio_row tbl_f2_hotkey_zipf:alpha_mem "$(m 10)" "$(m 25)" "$(m 50)" 1.15
load_narrow rev 2048
sql ch "UPDATE rev SET v1 = v1 + 1 WHERE id % 4 = 0" "BEGIN" "UPDATE rev SET v3 = v3 + 1 WHERE id = 4" "UPDATE rev SET v1 = v1 + 1 WHERE id = 4" "COMMIT" >/dev/null 2>&1
rev=$(problems rev)
row tbl_f2_hotkey_zipf:verify_rev_attnum 0 - "$rev" "$rev" 0
settled_rows tbl_f2_hotkey_zipf hot "$clean" "$tb0"
blocks_rows tbl_f2_hotkey_zipf "$clean"
sql ch "DROP TABLE hot" "DROP TABLE rev" >/dev/null
done_case tbl_f2_hotkey_zipf

# 3. tbl_f2_wide_upd: the indexed column and the text of a quarter of the rows, four times with a VACUUM after each; then 1,200 one-row updates while a repeatable read snapshot is open
CYCLE_SQL() { echo "UPDATE wid SET grp = grp + 100000, pad = repeat('y', 40) WHERE id % 4 = $(($1 - 1))"; }
N=16384
load_wide wid $N
clean=$(footprint wid)
tb0=$(temp_bytes)
sql ch "UPDATE wid SET grp = grp + 100000, pad = repeat('y', 40) WHERE id % 5 = 4" "VACUUM wid" >/dev/null || bail "the first update of wid failed"
churn_cycles tbl_f2_wide_upd wid "VACUUM wid"
cycle_rows tbl_f2_wide_upd
settled_rows tbl_f2_wide_upd wid "$clean" "$tb0"
load_wide snp 8192
coproc SNAP { psql -X -At -q -v ON_ERROR_STOP=1 -h "$(node_sock ch)" -p "$(port ch)" -U postgres -d "$(node_db ch)"; }
printf '%s\n' "BEGIN ISOLATION LEVEL REPEATABLE READ;" "SELECT count(*) AS c FROM snp \\gset" "\\echo READY" >&"${SNAP[1]}"
read -r -u "${SNAP[0]}" ready
sql ch "CHECKPOINT" >/dev/null
out=$({
	echo "SELECT pg_current_wal_lsn() AS l0 \\gset"
	for i in $(seq 800); do
		echo "UPDATE snp SET grp = grp + 1 WHERE id = $i;"
		(( i != 300 && i != 600 && i != 700 && i != 800 )) || echo "SELECT 'S $i ' || pg_wal_lsn_diff(pg_current_wal_lsn(), :'l0')::int8;"
	done
} | sql ch) || bail "the updates under a snapshot failed"
echo '\q' >&"${SNAP[1]}"
wait "$SNAP_PID"
s() { awk -v k="$1" '$1 == "S" && $2 == k { print $3 }' <<<"$out"; }
w300=$(s 300) w600=$(s 600) w700=$(s 700) w800=$(s 800)
ratio_row tbl_f2_wide_upd:snap_pend_del_wal "$((w300 / 300))" "$(((w600 - w300) / 300))" "$(((w800 - w700) / 100))" 1.25
sql ch "DROP TABLE snp" "DROP TABLE wid" >/dev/null
done_case tbl_f2_wide_upd

# 4. tbl_f2_del_ins: a quarter of the rows deleted and as many inserted, four times with a VACUUM after each
CYCLE_SQL() {
	echo "DELETE FROM di WHERE id > $((($1 - 1) * N / 4)) AND id <= $(($1 * N / 4))"
	echo "INSERT INTO di SELECT $((N + ($1 - 1) * N / 4)) + g, g % 64, g, g * 3, g % 1000, repeat('x', 40) FROM generate_series(1, $((N / 4))) g"
}
N=32768
load_narrow di $N
clean=$(footprint di)
tb0=$(temp_bytes)
churn_cycles tbl_f2_del_ins di "VACUUM di"
cycle_rows tbl_f2_del_ins
settled_rows tbl_f2_del_ins di "$clean" "$tb0"
parts=$(sql ch "SELECT count(*) FROM sift.parts('di')")
row tbl_f2_del_ins:parts_post 1 - "$parts" "$parts" 2
sql ch "DROP TABLE di" >/dev/null
done_case tbl_f2_del_ins

# 5. col_f2_churn: a heap table with a sift_col index that tracks changes: 15% narrow updates, 10% deletes and as many inserts, four times with a VACUUM after each
CYCLE_SQL() {
	echo "UPDATE hc SET a = a + 1 WHERE (id + $1 * 3) % 20 < 3"
	echo "DELETE FROM hc WHERE id <= $N AND id % 10 = $(($1 - 1))"
	echo "INSERT INTO hc SELECT $((N * $1)) + g, g, g * 3, g % 1000, repeat('x', 40) FROM generate_series(1, $((N / 10))) g"
}
N=16384
sql ch "CREATE TABLE hc (id int PRIMARY KEY, a bigint NOT NULL, b bigint NOT NULL, c int NOT NULL, pad text NOT NULL)" \
	"INSERT INTO hc SELECT g, g, g * 3, g % 1000, repeat('x', 40) FROM generate_series(1, $N) g" \
	"CREATE INDEX hc_col ON hc USING sift_col (id, a, b, c) WITH (track_changes = 'ctid')" "VACUUM hc" "SELECT sift.maintain('hc_col')" >/dev/null || bail "load hc failed"
clean=$(footprint hc_col)
tb0=$(temp_bytes)
churn_cycles col_f2_churn hc_col "VACUUM hc"
cycle_rows col_f2_churn
settled_rows col_f2_churn hc "$clean" "$tb0" hc_col
blocks_rows col_f2_churn "$clean"
sql ch "DROP TABLE hc" >/dev/null
done_case col_f2_churn

# 6. bm25_churn: a heap table with a BM25 index: a quarter of the documents rewritten, four times with a VACUUM after each, then the merge
CYCLE_SQL() { echo "UPDATE bd SET body = 'rev$1 ' || body WHERE id % 4 = $(($1 - 1))"; }
N=32768
sql ch "CREATE TABLE bd (id int PRIMARY KEY, body text NOT NULL)" \
	"INSERT INTO bd SELECT g, (SELECT string_agg('w' || (g * k % 997), ' ') FROM generate_series(1, 28) k) FROM generate_series(1, $N) g" \
	"CREATE INDEX bd_idx ON bd USING sift (body)" "VACUUM bd" >/dev/null || bail "load bd failed"
clean=$(footprint bd_idx)
tb0=$(temp_bytes)
churn_cycles bm25_churn bd_idx "VACUUM bd"
cycle_rows bm25_churn
settled_rows bm25_churn bd "$clean" "$tb0" bd_idx "sift.merge('bd_idx', false)" 262144
segs=$(sql ch "SELECT count(*) FROM sift.segments('bd_idx')")
row bm25_churn:segs_post 1 - "$segs" "$segs" 4
sql ch "DROP TABLE bd" >/dev/null
done_case bm25_churn

diag "the script took $SECONDS s"
finish
