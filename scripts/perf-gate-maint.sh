#!/usr/bin/env bash
# Performance gate of the maintenance path: the same workloads on two builds of sift (release flavor), in
# throwaway clusters of scripts/pgtest.sh, builds interleaved, --runs times per cell.
#
#   scripts/perf-gate-maint.sh [--rows N] [--runs N] [--secs N] [--cells REGEX] [--build LABEL=DIR]...
#
# Every --build is a checkout, e.g. --build old=/tmp/1.3.1 --build new=. (the first is the baseline, default
# new=this checkout). Cells, each on a fresh table:
#   insert16_tps / insert16_p99_ms  16 clients, one-row INSERT, --secs seconds
#   bulk_rows_s / copy_rows_s       INSERT .. SELECT and COPY FROM of --rows rows
#   merge_insert_tps / _p99_ms      the same inserts while the worker merges a backlog of 60 segments
#   merge_count_ms / merge_rank_ms  mean latency of a count and of a ranked query (4 clients) during VACUUM and merges
#   idle60_*_ms / idle1_*_ms        the same queries with no maintenance running, on 61 segments and on one
#   same_count_ms / same_rank_ms    the same queries while inserts, promotions and forced merges of the SQL functions run on both builds
#   vacseg<N>_s / _segs / _count_ms / _rank_ms  VACUUM time, segments and query latency after it at vacuum_segments N = 2, 4, 8, 16
#                                   (group vacseg, not in the default selection of a baseline without the option)
#   promote_s                       20 promotions of a mutable segment of 3000 documents of 40 words (about 4 MB)
#   vacuum_s / vacuum_segs          VACUUM of that backlog (it merges to a fixed point) and the segments it leaves;
#   after_vacuum_count_ms / _rank_ms  the two queries on the index after that VACUUM and 20000 more rows
#   merge_s / merge_temp_kb / merge_peak_mb  sift.merge() of that backlog; temporary bytes of the
#                                   merge; most anonymous memory the cluster's processes took above what they took before
#   churn_index_mb                  size of the index after the backlog is merged
#   create_s / create64_s           CREATE INDEX at the server defaults / at 64MB without workers
# --cells selects groups by name (insert, bulk, merge_insert, queries, idle, same, vacseg, promote, vacuum, merge, create), e.g. --cells "^(merge_insert|queries)$".
# The table ends with new/old per cell and marks, with "<<", a cell more than 2% worse (the *_tps and *_rows_s
# cells are better higher, the others lower); re-measure those with more --runs.
# Ports SIFT_PORT (default 56150) and up. The clusters and their directories are removed at the end.
set -Eeuo pipefail
trap 'echo "perf-gate: failed at line $LINENO: $BASH_COMMAND" >&2' ERR
export LC_ALL=C

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=100000 RUNS=3 SECS=20 CELLS=. BUILDS=()
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--runs) RUNS=$2; shift ;;
		--secs) SECS=$2; shift ;;
		--cells) CELLS=$2; shift ;;
		--build) BUILDS+=("$2"); shift ;;
		*) sed -n '2,25p' "$0"; exit 1 ;;
	esac
	shift
done
(( ${#BUILDS[@]} )) || BUILDS=("new=$REPO")
. "$REPO/scripts/multibuild.sh"
MB_TESTING=
mb_begin sift-perf-gate

now() { date +%s.%N; }
elapsed() { bc <<<"$(now) - $1"; }
# N random words (a skewed vocabulary); with a column of the outer query it is evaluated for every row, without once
words() { echo "(SELECT string_agg('w' || floor(power(random(), 3) * 50000)::int, ' ') FROM generate_series(1, $1) w${2:+ WHERE $2 IS NOT NULL})"; }
ROW="$(words 40)"
echo "INSERT INTO t (body) SELECT $ROW;" >"$BASE/insert.sql"
echo "SELECT count(*) FROM t WHERE body @@> 'w1 OR w2 OR w3';" >"$BASE/count.sql"
echo "SELECT id FROM t WHERE body @@> 'w1 OR w2 OR w3' ORDER BY sift.score(ctid) DESC LIMIT 10;" >"$BASE/rank.sql"

fresh() { # LABEL [index options, default $FRESH_OPTS]: empty table and index
	q "$1" "DROP TABLE IF EXISTS t; CREATE TABLE t (id serial, body text); CREATE INDEX t_idx ON t USING sift (body) ${2:-${FRESH_OPTS:-}}" >/dev/null
}
settle() { # LABEL: waits for the maintenance worker to finish
	until [[ $(q "$1" "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'") == 0 ]]; do sleep 0.2; done
}
backlog() { # LABEL: 60 promoted segments, the worker off
	q "$1" "SET sift.async_maintenance = off" "DO \$\$ BEGIN FOR k IN 1..60 LOOP
		INSERT INTO t (body) SELECT $(words 6 i) FROM generate_series(1, 1500) i; PERFORM sift.promote('t_idx'); COMMIT; END LOOP; END \$\$" >/dev/null
}
bench() { # LABEL PREFIX clients script secs: pgbench; prints PREFIX_tps and PREFIX_p99_ms (latency of the script PREFIX_lat_ms)
	local l=$1 p=$2 log=$BASE/$1.log out
	rm -f "$log".*
	out=$(pgbench -h "${TMP[$l]}/sock" -p "${PORT[$l]}" -U postgres -n -c "$3" -j 4 -T "$5" -l --log-prefix="$log" -f "$4" sift 2>&1)
	echo "${p}_tps $(sed -n 's/^tps = \([0-9.]*\).*/\1/p' <<<"$out" | head -1)"
	echo "${p}_lat_ms $(sed -n 's/^latency average = \([0-9.]*\) ms.*/\1/p' <<<"$out")"
	cat "$log".* | awk '{print $3}' | sort -n | awk '{a[NR] = $1} END {printf "%s_p99_ms %.2f\n", "'"$p"'", a[int(NR * 0.99) + 1] / 1000}'
}
anon_kb() { # anonymous memory of the processes of a cluster (children of its postmaster)
	local pm p
	pm=$(head -1 "${TMP[$1]}/data/postmaster.pid")
	for p in $(pgrep -P "$pm"); do awk '/^RssAnon/ {print $2}' "/proc/$p/status" 2>/dev/null || true; done | awk '{s += $1} END {print s + 0}'
}
peak_kb() { # LABEL: polls until the file $BASE/stop exists, prints the highest sample
	local best=0 kb
	while [[ ! -e $BASE/stop ]]; do
		kb=$(anon_kb "$1")
		(( kb > best )) && best=$kb
		sleep 0.05
	done
	echo "$best"
}
temp_kb() { q "$1" "SELECT pg_stat_force_next_flush(); SELECT temp_bytes / 1024 FROM pg_stat_database WHERE datname = 'sift'" | tail -1; }

# The cells, in groups that --cells selects by name: each is a function "LABEL: name value lines"
g_insert() {
	fresh "$1"
	bench "$1" insert16 16 "$BASE/insert.sql" "$SECS" | grep -v lat_ms
	settle "$1"
}
g_bulk() {
	local t0
	fresh "$1"
	t0=$(now); q "$1" "INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, $ROWS) i" >/dev/null
	echo "bulk_rows_s $(bc <<<"$ROWS / $(elapsed "$t0")")"
	settle "$1"
	q "$1" "COPY t (body) TO '$BASE/copy.csv' CSV" >/dev/null; fresh "$1"
	t0=$(now); q "$1" "COPY t (body) FROM '$BASE/copy.csv' CSV" >/dev/null
	echo "copy_rows_s $(bc <<<"$ROWS / $(elapsed "$t0")")"
	settle "$1"
}
g_merge_insert() {
	fresh "$1"; backlog "$1"
	bench "$1" merge_insert 16 "$BASE/insert.sql" "$SECS" | grep -v lat_ms
	settle "$1"
}
g_queries() {
	fresh "$1"; backlog "$1"; q "$1" "INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, 20000) i" >/dev/null; settle "$1"
	( for _ in $(seq 3); do q "$1" "DELETE FROM t WHERE id % 7 = 0" "VACUUM t" >/dev/null 2>&1 || true; done ) &
	bench "$1" merge_count 4 "$BASE/count.sql" 8 | grep lat_ms | sed 's/_lat_ms/_ms/'
	bench "$1" merge_rank 4 "$BASE/rank.sql" 8 | grep lat_ms | sed 's/_lat_ms/_ms/'
	wait
}
# The read path on its own: the same query latency with no maintenance running, on the same 60 segments, and on one
g_idle() {
	fresh "$1"; backlog "$1"; q "$1" "INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, 1000) i" "SELECT sift.promote('t_idx')" >/dev/null
	q "$1" "SELECT 'idle_segs ' || count(*) FROM sift.segments('t_idx')"
	bench "$1" idle60_count 4 "$BASE/count.sql" 5 | grep lat_ms | sed 's/_lat_ms/_ms/'
	bench "$1" idle60_rank 4 "$BASE/rank.sql" 5 | grep lat_ms | sed 's/_lat_ms/_ms/'
	q "$1" "SELECT sift.merge('t_idx', true)" >/dev/null
	bench "$1" idle1_count 4 "$BASE/count.sql" 5 | grep lat_ms | sed 's/_lat_ms/_ms/'
	bench "$1" idle1_rank 4 "$BASE/rank.sql" 5 | grep lat_ms | sed 's/_lat_ms/_ms/'
}
# The queries while the same maintenance runs on both builds (inserts, promotions and forced merges by the SQL functions)
g_same() {
	fresh "$1"; backlog "$1"; settle "$1"
	( for _ in $(seq 4); do q "$1" "SET sift.async_maintenance = off" "INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, 5000) i" \
		"SELECT sift.promote('t_idx')" "SELECT sift.merge('t_idx', true)" >/dev/null 2>&1 || true; done ) &
	bench "$1" same_count 4 "$BASE/count.sql" 8 | grep lat_ms | sed 's/_lat_ms/_ms/'
	bench "$1" same_rank 4 "$BASE/rank.sql" 8 | grep lat_ms | sed 's/_lat_ms/_ms/'
	wait
}
# VACUUM of the 60-segment backlog and the queries on what it leaves, at vacuum_segments 2, 4, 8 and 16 (builds that have the option)
g_vacseg() {
	local n t0
	for n in 2 4 8 16; do
		fresh "$1"; backlog "$1"
		q "$1" "ALTER INDEX t_idx SET (vacuum_segments = $n)" >/dev/null
		t0=$(now); q "$1" "VACUUM t" >/dev/null
		echo "vacseg${n}_s $(elapsed "$t0")"
		echo "vacseg${n}_segs $(q "$1" "SELECT count(*) FROM sift.segments('t_idx')")"
		bench "$1" vacseg${n}_count 4 "$BASE/count.sql" 4 | grep lat_ms | sed 's/_lat_ms/_ms/'
		bench "$1" vacseg${n}_rank 4 "$BASE/rank.sql" 4 | grep lat_ms | sed 's/_lat_ms/_ms/'
	done
}
g_promote() {
	fresh "$1"
	q "$1" "SET sift.async_maintenance = off" "DROP TABLE IF EXISTS acc" "CREATE TABLE acc (s float8)" \
		"DO \$\$ DECLARE t0 timestamptz; tot interval := 0; BEGIN FOR n IN 1..20 LOOP
			INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, 3000) i;
			t0 := clock_timestamp(); PERFORM sift.promote('t_idx'); tot := tot + (clock_timestamp() - t0); COMMIT; END LOOP;
			INSERT INTO acc VALUES (extract(epoch FROM tot)); END \$\$" >/dev/null
	echo "promote_s $(q "$1" "SELECT s FROM acc")"
}
g_vacuum() {
	local t0
	fresh "$1"; backlog "$1"
	t0=$(now); q "$1" "VACUUM t" >/dev/null
	echo "vacuum_s $(elapsed "$t0")"
	echo "vacuum_segs $(q "$1" "SELECT count(*) FROM sift.segments('t_idx')")"
	settle "$1"
	q "$1" "INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, 20000) i" "VACUUM t" >/dev/null; settle "$1"
	bench "$1" after_vacuum_count 4 "$BASE/count.sql" 5 | grep lat_ms | sed 's/_lat_ms/_ms/'
	bench "$1" after_vacuum_rank 4 "$BASE/rank.sql" 5 | grep lat_ms | sed 's/_lat_ms/_ms/'
}
# LABEL PREFIX STATEMENT...: runs the statements, prints PREFIX_s (time), PREFIX_peak_mb (most anonymous memory the cluster's
# processes took above what they took before) and PREFIX_temp_kb (temporary bytes written)
measured() {
	local l=$1 p=$2 t0 kb0 base poller
	shift 2
	kb0=$(temp_kb "$l")
	rm -f "$BASE/stop"; base=$(anon_kb "$l"); peak_kb "$l" >"$BASE/peak" &
	poller=$!
	t0=$(now); q "$l" "$@" >/dev/null
	echo "${p}_s $(elapsed "$t0")"
	touch "$BASE/stop"; wait "$poller"
	echo "${p}_peak_mb $(( ($(<"$BASE/peak") - base) / 1024 ))"
	echo "${p}_temp_kb $(( $(temp_kb "$l") - kb0 ))"
}
g_merge() {
	fresh "$1"; backlog "$1"
	measured "$1" merge "SELECT sift.merge('t_idx') FROM generate_series(1, 20)"
	settle "$1"
	echo "churn_index_mb $(q "$1" "SELECT pg_relation_size('t_idx') / 1048576.0")"
}
g_create() {
	local t0
	fresh "$1"; q "$1" "DROP INDEX t_idx; INSERT INTO t (body) SELECT $(words 150 i) FROM generate_series(1, $ROWS) i" >/dev/null
	t0=$(now); q "$1" "CREATE INDEX t_idx ON t USING sift (body)" >/dev/null
	echo "create_s $(elapsed "$t0")"
	q "$1" "DROP INDEX t_idx" >/dev/null
	t0=$(now); q "$1" "SET maintenance_work_mem = '64MB'; SET max_parallel_maintenance_workers = 0; CREATE INDEX t_idx ON t USING sift (body)" >/dev/null
	echo "create64_s $(elapsed "$t0")"
}
GROUPS_ALL=(insert bulk merge_insert queries idle same promote vacuum vacseg merge create)
cell() { # LABEL: the selected groups
	for g in "${GROUPS_ALL[@]}"; do
		[[ $g =~ $CELLS && ( $g != vacseg || $CELLS == *vacseg* ) ]] && "g_$g" "$1"
	done
	return 0
}

# Groups dead and mut (only with --cells dead / --cells mut): A/Bs of two values of a reloption inside one build (the order of the two alternates with
# the run, so that drifts of the load hit both).
#   dead<upd|del><P>_*  10 rounds of updating / deleting 6% of 4 x --rows rows, each followed by VACUUM, on an index built with dead_rewrite_pct = P
#                       (50, 5): vac_s (VACUUM time), vac_wal_mb (WAL of the VACUUM and the maintenance after it: the write amplification),
#                       idx_mb (size at the end), idx_max_mb (largest size after a round), segs, and the count / rank latency (ms) at the end
#   mut<L>_*            the same table at mutable_limit = L kB (4096, 1024): one-row INSERTs at 16 clients and at 1 (insert16_tps / _p99_ms,
#                       insert1_tps / _p99_ms), the worker's promotion and merge durations of that load from the server log (promo_ms / merge_ms,
#                       mean, and promo_n / merge_n), bulk_rows_s, copy_rows_s, then on the index that grew idx_mb, segs and the latency (ms)
#                       of a count, a ranked OR, a ranked AND and a single term at 4 clients
alt() { if (( r % 2 )); then echo "$1 $2"; else echo "$2 $1"; fi; }
# VACUUM seconds and the WAL bytes of the VACUUM and the maintenance that follows it, as "SECONDS BYTES"
timed_vacuum() { # LABEL
	local lsn0 t0 s
	settle "$1"; lsn0=$(q "$1" "SELECT pg_current_wal_lsn()")
	t0=$(now); q "$1" "VACUUM t" >/dev/null; s=$(elapsed "$t0")
	settle "$1"
	echo "$s $(q "$1" "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '$lsn0')")"
}
# A table of 4 x --rows rows with its index built (one segment) at dead_rewrite_pct = P, then 10 rounds of LOAD (upd|del) + VACUUM
dead_load() { # LABEL LOAD P
	local l=$1 load=$2 pct=$3 n=$((ROWS * 4)) r v s wal tv=0 tw=0 mx=0 mb p=dead$2$3
	q "$l" "DROP TABLE IF EXISTS t; CREATE TABLE t (id serial PRIMARY KEY, body text) WITH (autovacuum_enabled = off)" \
		"INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, $n) i" \
		"CREATE INDEX t_idx ON t USING sift (body) WITH (dead_rewrite_pct = $pct, build_segments = 1)" "VACUUM ANALYZE t" >/dev/null
	settle "$l"
	for r in $(seq 10); do
		if [[ $load == upd ]]; then q "$l" "UPDATE t SET body = $(words 40 id) WHERE id % 17 = $r" >/dev/null
		else q "$l" "DELETE FROM t WHERE id % 17 = $r" >/dev/null; fi
		read -r v wal < <(timed_vacuum "$l")
		tv=$(bc <<<"$tv + $v"); tw=$(bc <<<"$tw + $wal")
		mb=$(q "$l" "SELECT pg_relation_size('t_idx') / 1048576.0")
		(( $(bc <<<"$mb > $mx") )) && mx=$mb
	done
	echo "${p}_vac_s $tv"
	echo "${p}_vac_wal_mb $(bc <<<"$tw / 1048576")"
	echo "${p}_idx_mb $mb"
	echo "${p}_idx_max_mb $mx"
	echo "${p}_segs $(q "$l" "SELECT count(*) FROM sift.segments('t_idx')")"
	bench "$l" ${p}_count 4 "$BASE/count.sql" 4 | grep lat_ms | sed 's/_lat_ms/_ms/'
	bench "$l" ${p}_rank 4 "$BASE/rank.sql" 4 | grep lat_ms | sed 's/_lat_ms/_ms/'
}
g_dead() {
	local pct load
	[[ $CELLS == *dead* ]] || return 0
	for pct in $(alt 50 5); do
		for load in upd del; do dead_load "$1" "$load" "$pct"; done
	done
}
# The worker's promotions and merges in the server log since byte offset $2: "PREFIX_promo_ms", "_merge_ms" (mean) and the counts
log_durations() { # LABEL OFFSET PREFIX
	tail -c +"$(( $2 + 1 ))" "${TMP[$1]}/postgres.log" | awk -v p="$3" '
		/sift: promoted a sealed segment of/ { match($0, / in [0-9]+ ms/); a += substr($0, RSTART + 4, RLENGTH - 7); na++ }
		/sift: merged [0-9]+ segments of/    { match($0, / in [0-9]+ ms/); m += substr($0, RSTART + 4, RLENGTH - 7); nm++ }
		END { printf "%s_promo_ms %.2f\n%s_promo_n %d\n%s_merge_ms %.2f\n%s_merge_n %d\n", p, na ? a / na : 0, p, na, p, nm ? m / nm : 0, p, nm }'
}
mut_load() { # LABEL L: the groups insert and bulk, and the queries on what they grew, on an index at mutable_limit = L
	local l=$1 p=mut$2 off c
	FRESH_OPTS="WITH (mutable_limit = $2)"
	cat >"$BASE/and.sql" <<<"SELECT id FROM t WHERE body @@> 'w1 w5' ORDER BY sift.score(ctid) DESC LIMIT 10;"
	cat >"$BASE/term.sql" <<<"SELECT id FROM t WHERE body @@> 'w3' ORDER BY sift.score(ctid) DESC LIMIT 10;"
	off=$(stat -c %s "${TMP[$l]}/postgres.log")
	g_insert "$l" | sed "s/^/${p}_/"
	bench "$l" ${p}_insert1 1 "$BASE/insert.sql" "$SECS" | grep -v lat_ms
	settle "$l"
	log_durations "$l" "$off" "$p"
	g_bulk "$l" | sed "s/^/${p}_/"
	q "$l" "INSERT INTO t (body) SELECT $(words 40 i) FROM generate_series(1, $ROWS) i" >/dev/null; settle "$l"
	echo "${p}_idx_mb $(q "$l" "SELECT pg_relation_size('t_idx') / 1048576.0")"
	echo "${p}_segs $(q "$l" "SELECT count(*) FROM sift.segments('t_idx')")"
	for c in count rank and term; do bench "$l" ${p}_$c 4 "$BASE/$c.sql" 4 | grep lat_ms | sed 's/_lat_ms/_ms/'; done
	FRESH_OPTS=
}
g_mut() {
	local L
	[[ $CELLS == *mut* ]] || return 0
	q "$1" "ALTER SYSTEM SET sift.log_min_duration = 0" "SELECT pg_reload_conf()" >/dev/null
	for L in $(alt 4096 1024); do mut_load "$1" "$L"; done
	q "$1" "ALTER SYSTEM RESET sift.log_min_duration" "SELECT pg_reload_conf()" >/dev/null
}
GROUPS_ALL+=(dead mut)

# Group mergebig (only with --cells mergebig): sift.merge(.., true) of 4 big segments in one process, 4 x --rows documents of 40
# words (one term in 9 of 10, a 16MB budget): the memory and the time of a merge by the size of its segments.
#   mergebig_s / mergebig_peak_mb / mergebig_temp_kb / mergebig_index_mb  time; most anonymous memory the cluster's processes took
#                                   above what they took before; temporary bytes; size of the index (--rows 500000 is 2M documents)
g_mergebig() {
	[[ $CELLS == *mergebig* ]] || return 0
	fresh "$1"
	q "$1" "DROP INDEX t_idx" "INSERT INTO t (body) SELECT (CASE WHEN i % 10 < 9 THEN 'hot ' ELSE '' END) || $(words 40 i) FROM generate_series(1, $ROWS * 4) i" \
		"SET maintenance_work_mem = '16MB'; SET max_parallel_maintenance_workers = 0; CREATE INDEX t_idx ON t USING sift (body) WITH (build_segments = 4)" >/dev/null
	measured "$1" mergebig "SET maintenance_work_mem = '16MB'" "SELECT sift.merge('t_idx', true)"
	echo "mergebig_index_mb $(q "$1" "SELECT pg_relation_size('t_idx') / 1048576.0")"
}
GROUPS_ALL+=(mergebig)

mb_start
for l in "${LABELS[@]}"; do
	q "$l" "CREATE EXTENSION pg_sift" >/dev/null
	q "$l" "ALTER SYSTEM SET autovacuum = off" "SELECT pg_reload_conf()" >/dev/null
done
echo "load average $(cut -d' ' -f1-3 /proc/loadavg), $ROWS rows, $RUNS runs, $SECS s per pgbench"

: >"$BASE/all.tsv"
for r in $(seq "$RUNS"); do
	for b in "${BUILDS[@]}"; do
		l=${b%%=*}
		cell "$l" | sed "s/^/$l /" >>"$BASE/all.tsv"
	done
done

awk '
	{ k = $1 SUBSEP $2; if (!(($2) in seen)) { seen[$2] = 1; order[++nm] = $2 } if (!($1 in lab)) { lab[$1] = 1; labs[++nl] = $1 }
	  cnt[k]++; sum[k] += $3; sq[k] += $3 * $3 }
	END {
		printf "%-18s", "cell"; for (j = 1; j <= nl; j++) printf " %20s", labs[j]; if (nl > 1) printf " %8s", "ratio"; printf "\n"
		for (i = 1; i <= nm; i++) {
			c = order[i]; printf "%-18s", c
			for (j = 1; j <= nl; j++) {
				k = labs[j] SUBSEP c; n = cnt[k]; m = sum[k] / n
				se = n > 1 ? sqrt((sq[k] / n - m * m) * n / (n - 1) / n) : 0
				printf " %12.2f +- %-5.2f", m, se; mean[j] = m
			}
			if (nl > 1) {
				ratio = mean[1] > 0 ? mean[nl] / mean[1] : 0
				worse = (c ~ /_tps$|_rows_s$/) ? ratio < 0.98 : ratio > 1.02
				printf " %8.3f%s", ratio, worse ? " <<" : ""
			}
			printf "\n"
		}
	}' "$BASE/all.tsv"
