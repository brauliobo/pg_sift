#!/usr/bin/env bash
# Index waste under churn: rounds of mixed UPDATE-by-id (indexed, plain and text columns), DELETE and INSERT on one table a lane (heap, btree, col = sift_col,
# gin, bm25 = sift), each round followed by VACUUM and the maintenance of the index, and after every step the bytes of the index, the free blocks and dead data
# inside it, the WAL and dirtied blocks of the step and the read cost of its statements (bench/churn/waste.sql). A btree and a GIN index on the same rows
# are the reference. Scenarios besides the plain rounds: ONLINE=1 builds a second index with CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY while
# pgbench writers run (stage cic / reindex: wall seconds, WAL, bytes, invalid leftovers).
#
#   bench/churn/waste.sh run          one run, appended to waste.metric
#   bench/churn/waste.sh report       the table of the run (ROUND=last or all)
#
# WASTE_LOG: the log file of a cluster started with sift.log_min_duration = 0 (adds the merges, documents and bytes they wrote, a round).
# COL_OPTS, BM25_OPTS: reloptions of the col and bm25 lanes, e.g. "key_cut = 'off'", "write_buffer_limit = 1024".
# Settings: N rows (100000), ROUNDS (6), RATE percent of the live rows touched a round (4), MIX upd_idx:upd_plain:upd_body:del:ins (4:3:2:2:2), BATCH rows a
# transaction (1), LANES (heap btree col gin bm25), RUN label (the lane set and N), AV=on lets autovacuum do the VACUUM (the step waits for it), DB (waste).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
DB=${DB:-waste} N=${N:-100000} ROUNDS=${ROUNDS:-6} RATE=${RATE:-4} MIX=${MIX:-4:3:2:2:2} BATCH=${BATCH:-1} LANES=${LANES:-heap btree col gin bm25}
RUN=${RUN:-n$N} AV=${AV:-off}
export PGOPTIONS="-c client_min_messages=warning -c waste.av=$AV -c statement_timeout=900s"
q() { psql -X -q -At -d "$DB" -v ON_ERROR_STOP=1 "$@"; }

# add LANE ROUND STAGE KEY VALUE
add() { q -c "select waste.add('$RUN','$1',$N,$2,'$3','$4',$5)" >/dev/null; }

# step LANE ROUND STAGE SQL: seconds, WAL bytes and dirtied blocks of the statement
step() {
	local l=$1 r=$2 st=$3 nested=false; shift 3
	[[ $st != dml ]] || nested=true
	q -c "select pg_stat_statements_reset()" >/dev/null
	local l0 t0 t1 l1
	l0=$(q -c "select pg_current_wal_lsn()"); t0=$(date +%s.%N)
	psql -X -q -d "$DB" -v ON_ERROR_STOP=1 -c "$*" >/dev/null
	t1=$(date +%s.%N); l1=$(q -c "select pg_current_wal_lsn()")
	add "$l" "$r" "$st" secs "$(echo "$t1 - $t0" | bc -l)"
	add "$l" "$r" "$st" wal_bytes "$(q -c "select pg_wal_lsn_diff('$l1','$l0')")"
	add "$l" "$r" "$st" dirtied "$(q -c "select waste.dirtied($nested)")"
}

# quiet LANE ROUND: waits until no sift maintenance worker runs (with AV=on also no autovacuum worker on the table) and records what that background work
# cost as stage maint: seconds waited and WAL bytes written meanwhile (the WAL of the cluster: the lane's own table is the only one written)
quiet() {
	local l0 t0 t1 l1
	l0=$(q -c "select pg_current_wal_lsn()"); t0=$(date +%s.%N); sleep 0.3
	for _ in $(seq 1200); do
		[[ $(q -c "select count(*) from pg_stat_activity where backend_type = 'sift maintenance' or (backend_type = 'autovacuum worker' and query like '%w_$1%')") == 0 ]] && break
		sleep 0.1
	done
	t1=$(date +%s.%N); l1=$(q -c "select pg_current_wal_lsn()")
	add "$1" "$2" maint secs "$(echo "$t1 - $t0" | bc -l)"
	# the merges and promotions of the round in the server log (WASTE_LOG, needs sift.log_min_duration = 0): how many, and the documents and bytes they wrote
	if [[ -n ${WASTE_LOG:-} ]]; then
		# the cut chosen again by a rewrite (NOTICE of the worker goes to the log), and the cut the index holds now
		add "$1" "$2" maint recuts "$(tail -c +$((LOG0 + 1)) "$WASTE_LOG" | { grep -c "w_${1}_ix\": the rewrite chose again" || true; })"
		tail -c +$((LOG0 + 1)) "$WASTE_LOG" | { grep -E "sift: (merged|promoted) .* of index \"w_${1}_ix\"" || true; } |
			awk -v l="$1" -v r="$2" -v run="$RUN" 'BEGIN { n = 0; d = 0; b = 0 } { n++; for (i = 1; i <= NF; i++) { if ($(i + 1) ~ /^documents?,?$/) d += $i; if ($(i + 1) ~ /^bytes/) b += $i } }
				END { printf "%d %d %d\n", n, d, b }' | { read -r n d b; add "$1" "$2" maint merges "$n"; add "$1" "$2" maint merge_docs "$d"; add "$1" "$2" maint merge_bytes "$b"; }
	fi
	add "$1" "$2" maint wal_bytes "$(q -c "select pg_wal_lsn_diff('$l1','$l0')")"
}

# state LANE ROUND STAGE: the structure of the index
state() {
	[[ $1 != col || $3 != maint_state ]] || q -c "insert into waste.note select '$RUN', 'col', $N, $2, 'reloptions', array_to_string(reloptions, '; ') from pg_class where relname = 'w_col_ix'" >/dev/null
	 q -c "select k, v from waste.state('$1')" | while IFS='|' read -r k v; do add "$1" "$2" "$3" "$k" "$v"; done; }

run() {
	createdb "$DB" 2>/dev/null || true
	q -c "create extension if not exists pg_sift" -c "create extension if not exists pgstattuple" -c "create extension if not exists pageinspect" -c "create extension if not exists pg_stat_statements" -f "$HERE/waste.sql" >/dev/null
	local opts=${COL_OPTS:+ with ($COL_OPTS)}
	q -c "alter database $DB set waste.col_opts = '${opts//\'/\'\'}'"
	opts=${BM25_OPTS:+ with ($BM25_OPTS)}
	q -c "alter database $DB set waste.bm25_opts = '${opts//\'/\'\'}'"
	q -c "delete from waste.metric where run = '$RUN'"
	q -c "call waste.mkmaster($N)"
	local mix="{${MIX//:/,}}"
	for l in $LANES; do
		q -c "call waste.mklane('$l')"
		q -c "vacuum (analyze, freeze, disable_page_skipping) w_$l"
		[[ $AV != on ]] || q -c "alter table w_$l set (autovacuum_enabled = true, autovacuum_vacuum_scale_factor = 0.005, autovacuum_vacuum_threshold = 50)"
		state "$l" 0 maint_state
		[[ $l = heap ]] || q -c "select waste.reads('$RUN','$l',$N,0)" >/dev/null
	done
	for r in $(seq 1 "$ROUNDS"); do
		for l in $LANES; do
			[[ ${CHECKPOINT_EACH:-1} != 1 ]] || q -c checkpoint
			[[ -z ${WASTE_LOG:-} ]] || LOG0=$(stat -c %s "$WASTE_LOG")
			step "$l" "$r" dml "call waste.dml('$l', $r, $RATE, '$mix', $BATCH)"
			state "$l" "$r" dml_state
			[[ $AV == on ]] || step "$l" "$r" vacuum "vacuum w_$l"
			quiet "$l" "$r"
			state "$l" "$r" maint_state
			[[ $l = heap ]] || q -c "select waste.reads('$RUN','$l',$N,$r)" >/dev/null
		done
		echo "round $r done $(date +%T)" >&2
	done
	for l in $LANES; do
		case $l in col) q -c "select sift.col_verify('w_col_ix')" >/dev/null || echo "VERIFY FAILED $l" >&2 ;; bm25) q -c "select sift.verify('w_bm25_ix')" >/dev/null || echo "VERIFY FAILED $l" >&2 ;; esac
	done
}

report() {
	psql -X -d "$DB" -c "select lane, round, idx_bytes, round(free_blk) free_blk, round(dead_b) dead_b, segs, buf_rows, dml_wal, vac_wal, maint_wal, dml_dirt, vac_dirt,
		round(vac_s::numeric, 2) vac_s, round(maint_s::numeric, 2) maint_s, merges, merge_docs, merge_bytes from waste.round where run = '$RUN' order by lane, round"
}

# online: CREATE INDEX CONCURRENTLY of a second index and REINDEX INDEX CONCURRENTLY of the lane's index while rate-limited pgbench writers (WRITERS clients,
# RATE_TPS statements a second in all, bench/churn/online.sql) change the table; then an offline REINDEX for the reference size. Records stage cic and reindex:
# seconds, WAL bytes, bytes of the index, free blocks, indexes left invalid, and the writers' statements done.
online() {
	createdb "$DB" 2>/dev/null || true
	q -c "create extension if not exists pg_sift" -c "create extension if not exists pgstattuple" -c "create extension if not exists pageinspect" -c "create extension if not exists pg_stat_statements" -f "$HERE/waste.sql" >/dev/null
	q -c "delete from waste.metric where run = '$RUN'"
	[[ -n $(q -c "select to_regclass('waste.m')") ]] || q -c "call waste.mkmaster($N)"
	for l in $LANES; do
		[[ $l != heap ]] || continue
		q -c "call waste.mklane('$l')"; q -c "vacuum (analyze, freeze, disable_page_skipping) w_$l"
		local t=w_$l ddl2 pid
		ddl2=$(q -c "select replace(waste.ddl('$l', '$t'), 'create index ', 'create index concurrently ')" | sed "s/${t}_ix/${t}_ix2/")
		pgbench -n -c "${WRITERS:-2}" -R "${RATE_TPS:-1500}" -T 600 -D tbl=$t -D rows=$N -f "$HERE/online.sql" "$DB" >/tmp/waste-pgbench-$l.out 2>&1 & pid=$!
		sleep 2
		local w0; w0=$(q -c "select coalesce(sum(n_tup_ins + n_tup_upd + n_tup_del), 0) from pg_stat_user_tables where relname = '$t'")
		for st in cic reindex; do
			local l0 t0 t1 l1 sql
			if [[ $st = cic ]]; then sql="$ddl2"; else sql="reindex index concurrently ${t}_ix"; fi
			l0=$(q -c "select pg_current_wal_lsn()"); t0=$(date +%s.%N)
			psql -X -q -d "$DB" -v ON_ERROR_STOP=1 -c "set statement_timeout = 0" -c "$sql" >/dev/null || echo "ONLINE STEP FAILED $l $st" >&2
			t1=$(date +%s.%N); l1=$(q -c "select pg_current_wal_lsn()")
			add "$l" 1 "$st" secs "$(echo "$t1 - $t0" | bc -l)"
			add "$l" 1 "$st" wal_bytes "$(q -c "select pg_wal_lsn_diff('$l1','$l0')")"
			add "$l" 1 "$st" invalid "$(q -c "select count(*) from pg_index i join pg_class c on c.oid = i.indexrelid where i.indrelid = '$t'::regclass and not i.indisvalid")"
			add "$l" 1 "$st" idx_bytes "$(q -c "select pg_relation_size('${t}_ix$([[ $st = cic ]] && echo 2)')")"
		done
		kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true
		sleep 1.5
		add "$l" 1 cic writer_rows "$(q -c "select coalesce(sum(n_tup_ins + n_tup_upd + n_tup_del), 0) - $w0 from pg_stat_user_tables where relname = '$t'")"
		q -c "vacuum $t"; quiet "$l" 1
		add "$l" 1 after idx_bytes_live "$(q -c "select pg_relation_size('${t}_ix')")"
		add "$l" 1 after idx2_bytes_live "$(q -c "select pg_relation_size('${t}_ix2')")"
		q -c "drop index ${t}_ix2"
		local b0 b1; b0=$(q -c "select pg_relation_size('${t}_ix')"); q -c "reindex index ${t}_ix" >/dev/null; b1=$(q -c "select pg_relation_size('${t}_ix')")
		add "$l" 1 after idx_bytes_before_offline_reindex "$b0"; add "$l" 1 after idx_bytes_offline_reindex "$b1"
		echo "online $l done $(date +%T)" >&2
	done
}

case ${1:-run} in run) run ;; online) online ;; report) report ;; *) sed -n '2,15p' "$0"; exit 1 ;; esac
