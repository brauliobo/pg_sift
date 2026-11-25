#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# What the background maintenance does with 25% of churn when nobody calls VACUUM, sift.maintain or sift.merge (sift.async_maintenance on, autovacuum off), on 16384, 32768 and 65536 rows:
# the col_delta chain of a narrow UPDATE of a sift_table, its DELETE, the DELETE and the partial slab of a sift_col index with a scope, and the DELETE of a sift (BM25) index are merged away
# by the workers alone, in a time that grows by at most 4.5 for 4 times the rows, with at most 2.5 times as much WAL as the bytes they compact, no temp file, a worker that keeps at most
# 8 bytes more per row, and a table at most 1.35 times the size of a fresh build of its survivors. When all of it has settled, a 3 s window in which a reader holds a lock sees no WAL,
# no merge and no queued job. The CHURN_ROW comments are the table of the results: case.metric | 1x | 2x | 4x | growth factor | limit | verdict; each row is a check.
# The idle window is the pg_sleep of the reader: nothing may happen in it, so there is no state to wait for.
. "$(dirname "$0")/lib.bash"

done_testing
node_init ae 1 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
sift.backend_cache_kb = 2MB
maintenance_work_mem = 4MB
temp_file_limit = 1MB
log_temp_files = 0"
node_start ae || bail "start failed"
LOG=$(dir ae)/postgres.log
CAP_MS=${SIFT_AE_CAP_MS:-1000}   # a healthy merge of 65536 rows needs far less; a run that does not converge gives up here (12 waits)
declare -A V                                # V[case.metric.scale]
NOT_MEASURED=n/a                             # a value a gate fails on
SCALES=("1x:16384" "2x:32768" "4x:65536")

now_ms() { date +%s%3N; }
wal_since() { sql ae "SELECT CASE WHEN pg_current_wal_flush_lsn() > '$1' THEN (SELECT coalesce(sum(record_length), 0)::bigint FROM pg_get_wal_records_info('$1', pg_current_wal_flush_lsn())
	WHERE resource_manager NOT IN ('Standby', 'XLOG')) ELSE 0 END"; }
temp_bytes() { awk '/temporary file: path/ { n += $NF } END { print n + 0 }' "$LOG"; }
# snap REL [SQL...]: one session runs the statements and reads REL: S_USED blocks in use, S_BYTES of parts, S_DEAD pct, S_DIRTY rows, S_SLABS and the insert LSN S_LSN
snap() {
	local rel=$1; shift
	IFS='|' read -r S_USED S_BYTES S_DEAD S_DIRTY S_SLABS S_LSN <<<"$(sql ae "$@" "SELECT sp.used_blocks, greatest(sp.bytes, 8192), st.dead_pct, coalesce(st.dirty_rows, 0), st.l0_slabs,
		pg_current_wal_insert_lsn() FROM sift.space('$rel') sp, sift.status('$rel') st" | tail -n 1)"
}
# bm_dead_pct INDEX [SQL...]: the pct of the documents of the segments that are dead, after the statements
bm_dead_pct() { local rel=$1; shift; sql ae "$@" "SELECT round(coalesce(sum(dead_docs), 0) * 100.0 / greatest(sum(ndocs), 1), 2) FROM sift.segments('$rel')" | tail -n 1; }
ratio() { [[ $2 != 0 ]] || { echo "$NOT_MEASURED"; return; }; awk "BEGIN { printf \"%.2f\", $1 / $2 }"; }
# the KB the worker keeps at 4x more than at 1x, per row more (not measured when no worker was seen at 1x)
bytes_per_row() { [[ $1 != 0 ]] || { echo "$NOT_MEASURED"; return; }; awk "BEGIN { printf \"%.2f\", ($2 - $1) * 1024 / (65536 - 16384) }"; }
le() { [[ $1 != "$NOT_MEASURED" && $2 != "$NOT_MEASURED" ]] && awk "BEGIN { exit !($1 <= $2) }"; }
vmhwm_kb() { local p kb m=0; for p in ${1//,/ }; do kb=$(awk '/^VmHWM:/ { print $2 }' "/proc/$p/status" 2>/dev/null); (( ${kb:-0} <= m )) || m=$kb; done; echo "$m"; }

sql ae "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_walinspect" "CREATE TABLE pairs (member int4 NOT NULL, scope_key int4 NOT NULL)" "INSERT INTO pairs SELECT m, m % 3 + 1 FROM generate_series(1, 4) m" \
	"CREATE UNIQUE INDEX pairs_pk ON pairs (member, scope_key)" >/dev/null || bail "setup failed"

# settle REL... -- COND: waits until the queue holds no job of the relations and the SQL condition COND holds, at most CAP_MS; sets CONV_OK, CONV_MS and RSS_KB (the highest VmHWM of a sift worker seen)
settle() {
	local rels=() cond t0 out seen
	while [[ $1 != -- ]]; do rels+=("'$1'::regclass"); shift; done
	shift
	cond="NOT EXISTS (SELECT 1 FROM sift_internal.maint_queue() WHERE relation IN ($(IFS=,; echo "${rels[*]}"))) AND ($*)"
	t0=$(now_ms) RSS_KB=0 CONV_OK=0
	while :; do
		out=$(sql ae "SELECT ($cond) || '|' || coalesce((SELECT string_agg(pid::text, ',') FROM pg_stat_activity WHERE backend_type LIKE 'sift %'), '')")
		seen=$(vmhwm_kb "${out#*|}")
		(( seen <= RSS_KB )) || RSS_KB=$seen
		[[ ${out%%|*} != t ]] || { CONV_OK=1; break; }
		(( $(now_ms) - t0 < CAP_MS )) || break
		sleep 0.05
	done
	CONV_MS=$(( $(now_ms) - t0 ))
}

# the work of the workers after the churn (a ratio of a merge that did not end is not measured): the WAL since LSN0 over the bytes the churn left (DIRTY) and over the bytes left after the merge (AFTER),
# the temp bytes since TEMP0
work() { # CASE TAG LSN0 TEMP0 DIRTY AFTER
	V[$1.temp_bytes.$2]=$(( $(temp_bytes) - $4 ))
	(( CONV_OK )) || { V[$1.wa_merge.$2]=$NOT_MEASURED V[$1.wal_eff.$2]=$NOT_MEASURED; return; }
	local wal=$(wal_since "$3")
	V[$1.wa_merge.$2]=$(ratio "$wal" "$5"); V[$1.wal_eff.$2]=$(ratio "$wal" "$6")
}

run_scale() {
	local tag=$1 n=$2 q=$(( $2 / 4 )) lsn0 temp0 dirty base

	# 1. sift_table, one column of 25% of the rows changed: the chain of col_delta pages of the partition is folded
	snap "t_cd_$tag" "CREATE TABLE t_cd_$tag (id int8, grp int4, ts timestamptz, c1 int4, c2 int4, c3 int4, c4 int4, c5 int4, c6 int4, c7 int4, c8 text, c9 text) USING sift_table" \
		"SELECT sift.table_set_option('t_cd_$tag', 'key_order', 'grp, id')" \
		"INSERT INTO t_cd_$tag SELECT g, g % 4 + 1, timestamptz '2025-01-01' + g * interval '1 second', hashtext(g || 'a'), hashtext(g || 'b'), hashtext(g || 'c'), hashtext(g || 'd'),
			hashtext(g || 'e'), hashtext(g || 'f'), hashtext(g || 'g'), md5(g::text), md5(g || 'x') FROM generate_series(1, $n) g" "VACUUM t_cd_$tag"
	base=$S_USED; temp0=$(temp_bytes)
	snap "t_cd_$tag" "UPDATE t_cd_$tag SET c1 = c1 + 1 WHERE id <= $q"
	V[tbl_f2_narrow_cd.chain_blocks0.$tag]=$(( S_USED - base )); dirty=$S_BYTES; lsn0=$S_LSN
	settle "t_cd_$tag" -- "(SELECT used_blocks <= $base + 2 FROM sift.space('t_cd_$tag')) AND (SELECT coalesce(dirty_rows, 0) = 0 AND dead_pct < 15 FROM sift.status('t_cd_$tag'))"
	V[tbl_f2_narrow_cd.converged.$tag]=$CONV_OK; V[tbl_f2_narrow_cd.converge_ms.$tag]=$CONV_MS; V[tbl_f2_narrow_cd.worker_rss_kb.$tag]=$RSS_KB
	snap "t_cd_$tag"
	V[tbl_f2_narrow_cd.chain_blocks.$tag]=$(( S_USED - base )); V[tbl_f2_narrow_cd.dirty_rows.$tag]=$S_DIRTY
	work tbl_f2_narrow_cd "$tag" "$lsn0" "$temp0" "$dirty" "$S_BYTES"

	# 2. sift_table, 25% of the rows deleted: the dead rows are rewritten and the end of the file is given back
	sql ae "CREATE TABLE t_del_$tag (id int8, grp int4, v int4, pad text) USING sift_table" "SELECT sift.table_set_option('t_del_$tag', 'key_order', 'grp, id')" \
		"INSERT INTO t_del_$tag SELECT g, g % 4 + 1, hashtext(g::text), md5(g::text) FROM generate_series(1, $n) g" \
		"CREATE TABLE t_ref_$tag (id int8, grp int4, v int4, pad text) USING sift_table" "SELECT sift.table_set_option('t_ref_$tag', 'key_order', 'grp, id')" \
		"INSERT INTO t_ref_$tag SELECT g, g % 4 + 1, hashtext(g::text), md5(g::text) FROM generate_series($q + 1, $n) g" "VACUUM t_del_$tag" "VACUUM t_ref_$tag" >/dev/null || bail "t_del_$tag"
	temp0=$(temp_bytes)
	snap "t_del_$tag" "DELETE FROM t_del_$tag WHERE id <= $q"
	V[tbl_f2_delete.dead_pct0.$tag]=$S_DEAD; dirty=$S_BYTES; lsn0=$S_LSN
	settle "t_del_$tag" -- "(SELECT dead_pct = 0 FROM sift.status('t_del_$tag'))"
	V[tbl_f2_delete.converged.$tag]=$CONV_OK; V[tbl_f2_delete.converge_ms.$tag]=$CONV_MS
	snap "t_del_$tag"
	V[tbl_f2_delete.dead_pct.$tag]=$S_DEAD
	V[tbl_f2_delete.space_amp.$tag]=$(sql ae "SELECT round((SELECT relation_blocks FROM sift.space('t_del_$tag')) * 8192.0 / pg_relation_size('t_ref_$tag'), 2)")
	work tbl_f2_delete "$tag" "$lsn0" "$temp0" "$dirty" "$S_BYTES"

	# 3. sift_col with a scope: 25% of the rows deleted and 500 rows inserted, a slab with fewer rows than the 4096 that flush it
	sql ae "CREATE TABLE t_col_$tag (id int8, grp int4, ts timestamptz, val int4, pad text)" \
		"INSERT INTO t_col_$tag SELECT g, g % 4 + 1, timestamptz '2025-01-01' + g * interval '1 second', hashtext(g::text), md5(g::text) FROM generate_series(1, $n) g" \
		"CREATE INDEX idx_col_$tag ON t_col_$tag USING sift_col (grp, ts, val, pad) WITH (key_order = 'grp, ts', scope = 'sc: grp -> pairs(member, scope_key)')" \
		"VACUUM t_col_$tag" >/dev/null || bail "idx_col_$tag"
	temp0=$(temp_bytes)
	snap "idx_col_$tag" "DELETE FROM t_col_$tag WHERE id <= $q" "INSERT INTO t_col_$tag SELECT g + $n, 1, timestamptz '2025-02-01', g, md5(g::text) FROM generate_series(1, 500) g"
	V[col_f2_churn.slabs0.$tag]=$S_SLABS; V[col_f2_churn.dead_pct0.$tag]=$S_DEAD; dirty=$S_BYTES; lsn0=$S_LSN
	settle "idx_col_$tag" "t_col_$tag" -- "(SELECT l0_slabs = 0 AND dead_pct = 0 FROM sift.status('idx_col_$tag'))"
	V[col_f2_churn.converged.$tag]=$CONV_OK; V[col_f2_churn.converge_ms.$tag]=$CONV_MS
	snap "idx_col_$tag"
	V[col_f2_churn.dead_pct.$tag]=$S_DEAD; V[col_f2_churn.slabs.$tag]=$S_SLABS
	work col_f2_churn "$tag" "$lsn0" "$temp0" "$dirty" "$S_BYTES"

	# 4. sift (BM25), exactly 25% of the documents deleted and the heap vacuumed, which marks the documents dead and merges nothing
	V[bm25_delete_25.dead_pct0.$tag]=$(bm_dead_pct "idx_bm_$tag" "CREATE TABLE t_bm_$tag (id int8, body text)" \
		"INSERT INTO t_bm_$tag SELECT g, 'alpha beta gamma doc' || g % 500 FROM generate_series(1, $n) g" "CREATE INDEX idx_bm_$tag ON t_bm_$tag USING sift (body)" \
		"DELETE FROM t_bm_$tag WHERE id <= $q" "VACUUM t_bm_$tag")
	settle "idx_bm_$tag" -- "(SELECT coalesce(sum(dead_docs), 0) * 100 < greatest(coalesce(sum(ndocs), 1), 1) * 25 FROM sift.segments('idx_bm_$tag'))"
	V[bm25_delete_25.converged.$tag]=$CONV_OK; V[bm25_delete_25.converge_ms.$tag]=$CONV_MS; V[bm25_delete_25.dead_pct.$tag]=$(bm_dead_pct "idx_bm_$tag")
}

# gate CASE.METRIC RULE LIMIT TEXT [FLOOR]: one CHURN_ROW and one check. le: every scale is at most LIMIT; ge: at least; growth: the 4x value is at most LIMIT times the 1x value (at least FLOOR)
gate() {
	local k=$1 a=${V[$1.1x]:--} b=${V[$1.2x]:--} c=${V[$1.4x]:--} v=PASS g=- x
	case $2 in
		le) for x in $a $b $c; do [[ $x == - ]] || le "$x" "$3" || v=FAIL; done ;;
		ge) for x in $a $b $c; do [[ $x == - ]] || le "$3" "$x" || v=FAIL; done ;;
		growth) g=$(ratio "$c" "$(awk "BEGIN { print ($a > ${5:-1}) ? $a : ${5:-1} }")"); le "$g" "$3" || v=FAIL ;;
	esac
	diag "CHURN_ROW $k | $a | $b | $c | $g | $4 | $v"
	check "$k: $4" test "$v" = PASS
}

T0=$(now_ms)
for s in "${SCALES[@]}"; do run_scale "${s%%:*}" "${s##*:}"; diag "scale ${s%%:*} ran until $(( $(now_ms) - T0 )) ms"; done

# the bytes the worker keeps for each row more than at 1x
V[tbl_f2_narrow_cd.alpha_mem.4x]=$(ratio "${V[tbl_f2_narrow_cd.worker_rss_kb.4x]}" "${V[tbl_f2_narrow_cd.worker_rss_kb.1x]}")
V[tbl_f2_narrow_cd.rss_b_per_row.4x]=$(bytes_per_row "${V[tbl_f2_narrow_cd.worker_rss_kb.1x]}" "${V[tbl_f2_narrow_cd.worker_rss_kb.4x]}")

for c in tbl_f2_narrow_cd tbl_f2_delete col_f2_churn bm25_delete_25; do gate $c.converge_ms growth 4.5 "alpha_merge_time <= 4.5" 500; done
gate tbl_f2_narrow_cd.chain_blocks0 ge 4 "the UPDATE left a chain of at least 4 blocks to fold"
gate tbl_f2_narrow_cd.converged ge 1 "the chain is folded by the workers alone"
gate tbl_f2_narrow_cd.chain_blocks le 2 "no block of the chain is left"
gate tbl_f2_narrow_cd.dirty_rows le 0 "dirty_rows == 0"
gate tbl_f2_narrow_cd.wa_merge le 2.5 "WA_merge <= 2.5"
gate tbl_f2_narrow_cd.wal_eff le 2.5 "WAL_eff <= 2.5"
gate tbl_f2_narrow_cd.temp_bytes le 0 "temp_bytes == 0"
gate tbl_f2_narrow_cd.worker_rss_kb ge 1 "a worker ran"
gate tbl_f2_narrow_cd.alpha_mem le 1.15 "alpha_mem <= 1.15"
gate tbl_f2_narrow_cd.rss_b_per_row le 8 "beta_rss <= 8 B/row"
gate tbl_f2_delete.dead_pct0 ge 25 "the DELETE left 25% dead rows"
gate tbl_f2_delete.converged ge 1 "the dead rows are rewritten by the workers alone"
gate tbl_f2_delete.dead_pct le 0 "dead_pct == 0"
gate tbl_f2_delete.space_amp le 1.35 "SA <= 1.35"
gate tbl_f2_delete.wa_merge le 2.5 "WA_merge <= 2.5"
gate tbl_f2_delete.wal_eff le 2.5 "WAL_eff <= 2.5"
gate tbl_f2_delete.temp_bytes le 0 "temp_bytes == 0"
gate col_f2_churn.slabs0 ge 1 "the INSERT left a partial slab"
gate col_f2_churn.dead_pct0 ge 25 "the DELETE left 25% dead rows"
gate col_f2_churn.converged ge 1 "the slab is flushed and the dead rows rewritten by the workers alone"
gate col_f2_churn.slabs le 0 "l0_slabs == 0"
gate col_f2_churn.dead_pct le 0 "dead_pct == 0"
gate col_f2_churn.wa_merge le 2.5 "WA_merge <= 2.5"
gate col_f2_churn.wal_eff le 2.5 "WAL_eff <= 2.5"
gate col_f2_churn.temp_bytes le 0 "temp_bytes == 0"
gate bm25_delete_25.dead_pct0 ge 25 "the DELETE left 25% dead documents"
gate bm25_delete_25.converged ge 1 "the dead documents are rewritten by the workers alone"
gate bm25_delete_25.dead_pct le 24.99 "dead documents < 25%"

# everything has settled: a reader that holds AccessShareLock on the tables for 3 s sees the workers do nothing (the WAL of the standby snapshots and checkpoints is not theirs)
idle() { [[ $(sql ae "SELECT NOT EXISTS (SELECT 1 FROM sift_internal.maint_queue()) AND NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_type LIKE 'sift %')") == t ]]; }
check "the maintenance of all relations is idle" wait_for idle 3
since=$(sql ae "SELECT clock_timestamp()"); lsn0=$(sql ae "SELECT pg_current_wal_insert_lsn()")
sql ae "BEGIN" "SELECT count(*) FROM t_del_4x" "SELECT count(*) FROM t_cd_4x" "SELECT pg_sleep(3)" "COMMIT" >/dev/null
V[idle_3s_gate.idle_wal_b.4x]=$(wal_since "$lsn0")
V[idle_3s_gate.idle_merges.4x]=$(sql ae "SELECT count(*) FROM sift.status() WHERE last_maint_at > '$since'")
V[idle_3s_gate.idle_queue.4x]=$(sql ae "SELECT count(*) FROM sift_internal.maint_queue()")
gate idle_3s_gate.idle_wal_b le 0 "0 B of worker WAL in 3 s"
gate idle_3s_gate.idle_merges le 0 "0 merges in 3 s"
gate idle_3s_gate.idle_queue le 0 "0 queued jobs after 3 s"

out=$(sql ae "SELECT c.oid::regclass || ' ' || k.problem || ' ' || coalesce(k.detail, '') FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace,
	LATERAL sift.check(c.oid::regclass) k WHERE n.nspname = 'public' AND c.relname ~ '^(t_cd|t_del|t_ref|idx_col|idx_bm)_'")
check "sift.check finds nothing in the 15 relations" test -z "$out"

finish
