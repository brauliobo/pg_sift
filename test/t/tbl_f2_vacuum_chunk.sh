#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The maintenance of a sift_table holds a memory that follows its budget and not its size (Spec 95, WS-95-1b). Every case is built at 1x, 2x and 4x of 32768 rows (8 to 10 times
# compressible payloads, parts written under a 64 MB maintenance_work_mem) and measured in a backend of its own at a 2 MB budget under temp_file_limit = 1MB:
#   alpha_mem  = peak(4x) / peak(1x), the peaks above a floor of the budget; at most 1.15            beta_exact = the slope of the bytes the memory scopes counted; at most 2 B/row
#   beta_rss   = the slope of the anonymous memory of the backend; at most 8 B/row                    temp_bytes = pg_stat_database.temp_bytes the case wrote; none
#   alpha_disk = the table after the case over the table before it; at most 2 (the parts that a VACUUM writes are beside the ones they replace)     alpha_time = time(4x) / time(1x); at most 5.5
# vacuum_dead / vacuum_freeze: VACUUM and VACUUM FREEZE settle a partition of parts that each hold more than the budget in chunks of the budget (PROD-95-01); freeze_cap: sift.vacuum_rewrite_limit
# never stops a pass of VACUUM FREEZE: one pass freezes every part, the oldest transaction first, and pg_class.relfrozenxid follows; vm_span: the pass of the visibility map reads the addresses of a part that spans a
# map page for each of the pages and not the columns (R1-VAC-02); cic_partial: the validation of CREATE INDEX CONCURRENTLY merges what the scan gave in windows of addresses and writes
# no temp file (R1-CREATE-02); par_scan: the images of a parallel scan are freed once the scan is set up (R1-READ-01).
. "$(dirname "$0")/lib.bash"

[[ -r /proc/self/status ]] || { echo '1..0 # SKIP needs /proc'; exit 0; }
N1=32768
SCALES=(1 2 4)
BUDGET=2097152
MWM=2MB
PAYLOAD="md5(g::text) || md5((g * 7)::text) || repeat('seg=' || lpad((g % 16)::text, 2, '0') || '|grp=' || lpad((g % 128)::text, 3, '0') || '|pad=0123456789abcdef|', 8)"

done_testing
node_init vc 47 "shared_preload_libraries = 'pg_sift'
wal_level = replica
autovacuum = off
shared_buffers = 64MB
max_wal_size = 1GB
max_parallel_workers_per_gather = 0
sift.async_maintenance = off
sift.test_create_format1 = off
sift.shared_cache_kb = 0"
node_start vc || bail "start failed"
sql vc "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

rows_of() { echo $((N1 * $1)); }
load() { sql vc "SET maintenance_work_mem = '64MB'" "CREATE TABLE $1 (id bigint, k_lo int, payload text) USING sift_table" "INSERT INTO $1 SELECT g, g % 8, $PAYLOAD FROM generate_series(1, $2) g" >/dev/null; }
stats_now() { sql vc "SELECT pg_stat_clear_snapshot()" "SELECT temp_bytes || ' ' || (blks_hit + blks_read) FROM pg_stat_database WHERE datname = 'postgres'" | tail -n 1; }
unfrozen() { sql vc "SELECT count(*) FILTER (WHERE unfrozen) FROM sift.parts('$1')"; }
digest() { sql vc "SELECT count(*) || '/' || coalesce(sum(id), 0) FROM $1"; }
relfrozen() { sql vc "SELECT relfrozenxid::text::bigint FROM pg_class WHERE relname = '$1'"; }
field() { sed -n "s/^$1 //p" <<<"$MOUT" | tail -n 1; }

# measure TABLE SETTINGS OPERATION...: one backend sets the settings, runs the operation between a memory peak and a clock, and tells the peak (kB), the bytes the scopes counted and the seconds
measure() {
	local tbl=$1 settings=$2
	shift 2
	MOUT=$(sql vc "SET maintenance_work_mem = '$MWM'" "SET temp_file_limit = '1MB'" "$settings" "SELECT sift._test_thp_disable()" "SELECT sift._test_malloc_stats(true) IS NOT NULL" \
		"SELECT sift._test_mem_highwater(true) IS NULL" "SELECT 'SZ0 ' || pg_relation_size('$tbl')" "SELECT 'BASE ' || sift._test_rss_peak_start()" "SELECT 'T0 ' || extract(epoch FROM clock_timestamp())" "$@" \
		"SELECT 'T1 ' || extract(epoch FROM clock_timestamp())" "SELECT 'PEAK ' || sift._test_rss_peak_stop()" "SELECT 'SZ1 ' || pg_relation_size('$tbl')" \
		"SELECT 'EXACT ' || coalesce(sum(peak), 0) FROM sift._test_mem_highwater(false) WHERE parent IS NULL" 2>&1)
}

declare -A PEAK EXACT SECS IOB DISK want
TEMP=0
FAILED=
record() {
	local m=$1 before=$2 after
	after=$(stats_now)
	if [[ $MOUT != *"EXACT "* ]]; then
		FAILED="$FAILED $m: $(grep -m1 -E 'ERROR|FATAL' <<<"$MOUT" | cut -c1-100)"
		return 1
	fi
	PEAK[$m]=$(( $(field PEAK) - $(field BASE) ))
	EXACT[$m]=$(field EXACT)
	DISK[$m]=$(awk -v a="$(field SZ0)" -v b="$(field SZ1)" 'BEGIN { printf "%.3f", b / a }')
	SECS[$m]=$(awk -v a="$(field T0)" -v b="$(field T1)" 'BEGIN { printf "%.3f", b - a }')
	TEMP=$(( TEMP > ${after% *} - ${before% *} ? TEMP : ${after% *} - ${before% *} ))
	IOB[$m]=$(( ${after#* } - ${before#* } ))
}

# verdict CASE ALPHA_DISK: the table row of the case and ok / not ok on every threshold; a case that failed to run is the error it ended with
verdict() {
	local name=$1 disk=${2:--} row v
	if [[ -n $FAILED ]]; then
		row=$(printf 'GATE_ROW %-18s | FAIL [%s]' "$name" "${FAILED# }")
		diag "$row"
		not_ok "$name: $FAILED"
		FAILED=
		return
	fi
	row=$(awk -v n="$name" -v b="$BUDGET" -v tmp="$TEMP" -v disk="$disk" -v dn=$((N1 * 3)) \
		-v p1="${PEAK[1]}" -v p2="${PEAK[2]}" -v p4="${PEAK[4]}" -v e1="${EXACT[1]}" -v e2="${EXACT[2]}" -v e4="${EXACT[4]}" -v t1="${SECS[1]}" -v t4="${SECS[4]}" -v i1="${IOB[1]}" -v i4="${IOB[4]}" '
		function mx(a, c) { return a > c ? a : c }
		BEGIN {
			p1 *= 1024; p2 *= 1024; p4 *= 1024
			fr = mx(b, 2097152); fe = mx(b, 1048576)
			a_rss = mx(p4, fr) / mx(p1, fr); a_ex = mx(e4, fe) / mx(e1, fe)
			a = (e4 > 0 && a_ex > a_rss) ? a_ex : a_rss
			br = (p4 - p1) / dn; be = (e4 > 0) ? (e4 - e1) / dn : 0
			at = t4 / mx(t1, 0.05); ai = mx(i4, 200) / mx(i1, 200)
			bad = ""
			if (a > 1.15) bad = bad sprintf(" alpha_mem=%.2f>1.15", a)
			if (be > 2.0) bad = bad sprintf(" beta_exact=%.1f>2.0", be)
			if (br > 8.0) bad = bad sprintf(" beta_rss=%.1f>8.0", br)
			if (tmp > 0) bad = bad sprintf(" temp_bytes=%d>0", tmp)
			if (disk != "-" && disk > 2.0) bad = bad sprintf(" alpha_disk=%.2f>2.0", disk)
			if (at > 5.5) bad = bad sprintf(" alpha_time=%.2f>5.5", at)
			if (ai > 5.5) bad = bad sprintf(" alpha_io=%.2f>5.5", ai)
			printf "GATE_ROW %-14s | %6.1fMB | %6.1fMB | %6.1fMB | %5.2f | %6.1f | %6.1f | %d | %s | %5.2f (%.3fs, %.3fs) | io %5.2f | %s", n, p1 / 1048576, p2 / 1048576, p4 / 1048576, a, be, br, tmp, disk, at, t1, t4, ai, (bad == "" ? "PASS" : "FAIL [" substr(bad, 2) "]")
		}')
	diag "$row"
	if [[ $row == *" PASS" ]]; then ok "$name: constant memory, no temp file, linear time"; else not_ok "$name: ${row##*| }"; fi
	TEMP=0
}

burn_vblocks() {
	PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -h "$(dir vc)/sock" -p "$(port vc)" -U postgres -d postgres \
		-c "BEGIN" -c "COPY $1 FROM PROGRAM 'seq -f \"-%g,0,x\" 1 100' (DELIMITER ',')" -c "ROLLBACK" >/dev/null || bail "the burn of virtual blocks failed"
}


# ---- vacuum_dead and vacuum_freeze: PROD-95-01
for kind in vacuum_dead vacuum_freeze; do
	for s in "${SCALES[@]}"; do
		t=${kind}_$s
		load "$t" "$(rows_of "$s")" || bail "load $t failed"
		[[ $kind == vacuum_dead ]] && { sql vc "DELETE FROM $t WHERE k_lo < 2" >/dev/null || bail "delete failed"; }
		want[$s]=$(digest "$t")
		before=$(stats_now)
		old_xid=$(relfrozen "$t")
		[[ $kind == vacuum_dead ]] && op="VACUUM $t" || op="VACUUM FREEZE $t"
		measure "$t" "SELECT 1" "$op"
		record "$s" "$before"
		if [[ -z $FAILED ]]; then
			[[ ${want[$s]} == "$(digest "$t")" ]] || FAILED="$FAILED $s: the rows changed"
			[[ $kind == vacuum_dead ]] || { [[ $(unfrozen "$t") == 0 && $(relfrozen "$t") -gt $old_xid ]] || FAILED="$FAILED $s: parts left unfrozen or relfrozenxid not moved"; }
		fi
	done
	verdict "$kind" "${DISK[4]:--}"
done

# ---- freeze_cap: a pass of VACUUM FREEZE is not stopped by sift.vacuum_rewrite_limit and relfrozenxid moves with it
CAP=freeze_cap
sql vc "SET maintenance_work_mem = '64MB'" "CREATE TABLE $CAP (id bigint, k_lo int, payload text) USING sift_table" >/dev/null || bail "create $CAP failed"
for q in 0 1 2 3; do
	sql vc "SET maintenance_work_mem = '64MB'" "INSERT INTO $CAP SELECT g, g % 8, $PAYLOAD FROM generate_series($((q * N1 + 1)), $(((q + 1) * N1))) g" >/dev/null || bail "insert $CAP failed"
done
want_cap=$(digest $CAP)
xid=$(relfrozen $CAP)
xid0=$xid
monotonic=1
first_left=
rewritten_max=0
passes=0
while [[ $(unfrozen $CAP) != 0 && $passes -lt 12 ]]; do
	gen=$(sql vc "SELECT coalesce(max(part_gen), 0) FROM sift.parts('$CAP')")
	sql vc "SET maintenance_work_mem = '$MWM'" "SET sift.vacuum_rewrite_limit = 1" "VACUUM FREEZE $CAP" >/dev/null || bail "capped pass failed"
	passes=$((passes + 1))
	new_xid=$(relfrozen $CAP)
	((new_xid >= xid)) || monotonic=0
	xid=$new_xid
	written=$(sql vc "SELECT coalesce(sum(bytes), 0) FROM sift.parts('$CAP') WHERE part_gen > $gen")
	((written > rewritten_max)) && rewritten_max=$written
	[[ -n $first_left ]] || first_left=$(unfrozen $CAP)
done
diag "freeze_cap: $passes passes, first pass left $first_left unfrozen parts, $rewritten_max bytes written by a pass, relfrozenxid $xid"
check "freeze_cap: the first pass freezes every part whatever sift.vacuum_rewrite_limit says" test "${first_left:-1}" -eq 0
check "freeze_cap: one pass is enough ($passes)" test "$passes" -eq 1
check "freeze_cap: relfrozenxid never goes back and the pass moves it" test "$monotonic" == 1 -a "$xid" -gt "$xid0"
check "freeze_cap: the pass ends with every part frozen and the rows as they were" test "$(unfrozen $CAP)" == 0 -a "$(digest $CAP)" == "$want_cap"

# ---- vm_span: R1-VAC-02, a part that spans 8, 16 and 32 pages of the visibility map (a session that rolls back a COPY takes 8192 virtual blocks of the table for good; 4 of them are a page of the map)
for s in "${SCALES[@]}"; do
	t=vm_span_$s
	load "$t" "$(rows_of "$s")" || bail "load $t failed"
	for ((b = 0; b < 32 * s; b++)); do
		burn_vblocks "$t"
	done
	sql vc "SET maintenance_work_mem = '1GB'" "INSERT INTO $t SELECT g + $(rows_of "$s"), g % 8, $PAYLOAD FROM generate_series(1, $(rows_of "$s")) g" "VACUUM FREEZE $t" >/dev/null || bail "vm_span $s prepare failed"
	want[$s]=$(digest "$t")
	before=$(stats_now)
	measure "$t" "SELECT 1" "VACUUM $t"
	record "$s" "$before"
	[[ ${want[$s]} == "$(digest "$t")" ]] || FAILED="$FAILED $s: the rows changed"
done
verdict vm_span

# ---- cic_partial: R1-CREATE-02, a partial index whose validation has nothing to merge with
for s in "${SCALES[@]}"; do
	t=cic_$s
	n=$(rows_of "$s")
	load "$t" "$n" || bail "load $t failed"
	before=$(stats_now)
	measure "$t" "SET maintenance_work_mem = '1MB'" "CREATE INDEX CONCURRENTLY ${t}_i ON $t USING btree (k_lo) WHERE id > $((n - 100))"
	record "$s" "$before"
	if [[ -z $FAILED ]]; then
		via_index=$(sql vc "SET enable_seqscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM $t WHERE id > $((n - 100)) AND k_lo >= 0")
		[[ $via_index == 100 ]] || FAILED="$FAILED $s: the index finds $via_index of 100 rows"
	fi
done
verdict cic_partial

# ---- par_scan: R1-READ-01, twenty parallel scans in one transaction leave nothing in the memory of the transaction
declare -A LEAK
CTX="SELECT 'TTC ' || sum(used_bytes) FROM pg_backend_memory_contexts WHERE name = 'TopTransactionContext'"
for s in "${SCALES[@]}"; do
	t=par_scan_$s
	load "$t" "$(rows_of "$s")" || bail "load $t failed"
	scans=()
	for _ in $(seq 20); do scans+=("SELECT count(*), sum(length(payload)) FROM $t"); done
	out=$(sql vc "SET sift.enable_agg = off" "SET max_parallel_workers_per_gather = 2" "SET parallel_setup_cost = 0" "SET parallel_tuple_cost = 0" \
		"SET min_parallel_table_scan_size = 0" "SET debug_parallel_query = on" "BEGIN" "$CTX" "${scans[@]}" "$CTX" "COMMIT" 2>&1)
	LEAK[$s]=$(sed -n 's/^TTC //p' <<<"$out" | awk 'NR == 1 { a = $1 } END { print $1 - a }')
done
diag "par_scan: bytes of TopTransactionContext left by 20 parallel scans: 1x ${LEAK[1]}, 2x ${LEAK[2]}, 4x ${LEAK[4]}"
check "par_scan: twenty parallel scans leave under 2 KB in the transaction at every size" test "${LEAK[1]:-9999999}" -lt 2048 -a "${LEAK[2]:-9999999}" -lt 2048 -a "${LEAK[4]:-9999999}" -lt 2048

finish
