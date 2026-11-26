#!/usr/bin/env bash
# The shared caches of the planner and the scan and the shared counters (col/leafshm.c, col/dictshm/dictshm.c, agg/stats.c) under many backends at once: the statistics of an
# index and the parsed dictionaries are read by every backend of a server for each statement that plans or scans, so a lock that all of them take is a queue. Each kind of
# access is run by one client and then by CLIENTS clients that start together (pgbench), while two writers keep invalidating and entering records of the same tables: the
# operations of all the clients in a second must be a multiple of what one client does, not less of it. Then CLIENTS clients plan and run SiftAgg scans of a table whose
# dictionary is shared, and sift.stats() must add up exactly what they and the counters of the hammer counted.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

CLIENTS=$(knob S103_CLIENTS 32 16)
NKEYS=256
OPS=$(knob S103_OPS 250000 100000)
QUERIES=$(knob S103_QUERIES 150 60)
MIN_SCALE=${S103_MIN_SCALE:-$((CORES >= 8 ? 4 : 1))}

plan 9
node_init primary 2 "sift.shared_cache_kb = ${S103_CACHE_KB:-65536}
max_connections = 100"
node_start primary || bail "primary start failed"
{
	echo "CREATE EXTENSION pg_sift; CREATE EXTENSION pg_sift_test;"
	echo "CREATE FUNCTION public.hammer(kind text, n int8, nkeys int4, max_ms int4, OUT ops int8, OUT found int8, OUT us int8, OUT longest_us int8)
		AS '\$libdir/pg_sift', 'sift_test_shm_caches_hammer' LANGUAGE C STRICT;"
	echo "CREATE TABLE t AS SELECT g AS id, 'k' || (g % 40) AS s FROM generate_series(1, 20000) g;"
	echo "CREATE INDEX t_col ON t USING sift_col (id, s) WITH (rollup_columns = 'off');"
	echo "VACUUM (FREEZE, ANALYZE) t;"
} | sql primary >/dev/null || bail "setup failed"
D=$(dir primary)

# the operations a second of CLIENTS pgbench clients do of KIND: each runs N of them in one transaction, so the tps of pgbench times N
rate() {
	local kind=$1 clients=$2 n=$3 tps
	echo "SELECT * FROM hammer('$kind', $n, $NKEYS, 0);" >"$D/$kind.sql"
	tps=$(pgbench -n -f "$D/$kind.sql" -c "$clients" -j 8 -t 1 -h "$D/sock" -p "$(port primary)" -U postgres postgres 2>&1 | sed -n 's/^tps = \([0-9.]*\) .*/\1/p')
	awk -v t="$tps" -v n="$n" 'BEGIN { printf "%d", t * n }'
}
scale() { awk -v a="$2" -v b="$1" 'BEGIN { printf "%.2f", (b > 0) ? a / b : 0 }'; }
hammer() { sql primary "SELECT ops || ',' || found || ',' || us || ',' || longest_us FROM hammer('$1', $2, $NKEYS, ${3:-0})"; }

sql primary "SELECT ops FROM hammer('prepare', 0, $NKEYS, 0)" >/dev/null
IFS=, read -r leaf_ops leaf_found _ _ <<<"$(hammer leaf_read 20000)"
IFS=, read -r dict_ops dict_found _ _ <<<"$(hammer dict_read 20000)"
# S103_CACHE_KB: a ring so small that the writers wrap it under the readers, which then find some of their records
if [[ -z ${S103_CACHE_KB:-} ]]; then
	check "the records the hammer entered are found by their readers" test "$leaf_found|$dict_found" == "$leaf_ops|$dict_ops"
else
	check "the records the hammer entered are found by some readers" test "$leaf_found" -gt 0 -a "$dict_found" -gt 0
fi

start_writers() {
	for w in leaf_write dict_write; do
		echo "SELECT * FROM hammer('$w', 1000000000, $NKEYS, 100);" >"$D/$w.sql"
		pgbench -n -f "$D/$w.sql" -c 1 -T 120 -h "$D/sock" -p "$(port primary)" -U postgres postgres >"$D/$w.log" 2>&1 &
		WRITERS+=($!)
	done
}
stop_writers() { kill "${WRITERS[@]}" 2>/dev/null; wait "${WRITERS[@]}" 2>/dev/null; WRITERS=(); }
WRITERS=()
start_writers
declare -A ONE MANY
for kind in leaf_read dict_read stats; do
	ONE[$kind]=$(rate "$kind" 1 "$OPS")
	MANY[$kind]=$(rate "$kind" "$CLIENTS" "$OPS")
	diag "$kind: 1 client ${ONE[$kind]} ops/s, $CLIENTS clients ${MANY[$kind]} ops/s, x$(scale "${ONE[$kind]}" "${MANY[$kind]}")"
done
stop_writers
for kind in leaf_read dict_read stats; do
	check "$CLIENTS clients of $kind do at least $MIN_SCALE times the operations of one, with two writers on the caches" \
		awk -v s="$(scale "${ONE[$kind]}" "${MANY[$kind]}")" -v m="$MIN_SCALE" 'BEGIN { exit !(s >= m) }'
done

# CLIENTS backends plan and scan a table whose dictionary the ring holds: the scans of one transaction are counted by sift.stats() once each
scans() { sql primary "SELECT value FROM sift.stats() WHERE name = 'scans'"; }
cat >"$D/storm.sql" <<'SQL'
LOAD 'pg_sift';
SET sift.agg_force = on;
SET sift.test_col_dict_shared = 1;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS off) SELECT count(*) FROM t;
SELECT count(*) FROM (SELECT s FROM t WHERE s LIKE '%1%' GROUP BY s) x;
SQL
storm() { pgbench -n -f "$D/storm.sql" -c "$1" -j 8 -t "$2" -h "$D/sock" -p "$(port primary)" -U postgres postgres 2>&1; }
S0=$(scans)
storm 1 1 >/dev/null
PER_TX=$(($(scans) - S0))
check "a transaction of the storm is counted by sift.stats() as a SiftAgg scan" test "$PER_TX" -ge 1
S1=$(scans)
out=$(storm "$CLIENTS" "$QUERIES")
diag "storm: $(grep -E '^(tps|latency average)' <<<"$out" | tr '\n' ' ')"
S2=$(scans)
check "the storm of $CLIENTS clients failed no statement" grep -q '^number of failed transactions: 0' <<<"$out"
check "sift.stats() counts exactly the scans of the clients" test "$((S2 - S1))" == "$((PER_TX * CLIENTS * QUERIES))"
H0=$(scans)
rate stats "$CLIENTS" 1000 >/dev/null
check "and exactly the counts of $CLIENTS clients that add to it at once" test "$(($(scans) - H0))" == "$((CLIENTS * 1000))"
check "no record is left pinned by the clients that read in place" test "$(sql primary "SELECT records || '|' || pins FROM sift._test_col_dict_pinned()")" == "0|0"
