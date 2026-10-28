#!/usr/bin/env bash
# The size of the ring of shared dictionaries (sift.col_dict_shared_cache_kb, col/dictshm.c): 0 makes no segment and shares nothing, a size is the size of the ring
# (one segment of that many bytes, made by the first backend that keeps a dictionary), a ring too small for the dictionary is never made, and a backend that sets another
# size after the ring is made uses the ring as it is. The rows are those of the stock plan in every case. Observed through the segments of /dev/shm (dynamic_shared_memory_type = posix).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

[[ -d /dev/shm && -r /proc/self/maps ]] || { echo "1..0 # SKIP needs /dev/shm and /proc (Linux)"; exit 0; }
plan 10
node_init primary 3 "sift.col_dict_shared_cache_kb = 0"
node_start primary || bail "primary start failed"
{
	echo "CREATE EXTENSION pg_sift;"
	echo "CREATE TABLE t1 AS SELECT g AS id, 'k' || md5(g::text) AS s FROM generate_series(1, 20000) g;"
	echo "CREATE INDEX t1_col ON t1 USING sift_col (id, s) WITH (rollup_dims = 'off');"
	echo "VACUUM (FREEZE, ANALYZE) t1;"
} | sql primary >/dev/null || bail "setup failed"
[[ $(sql primary "SHOW dynamic_shared_memory_type") == posix ]] || { echo "1..0 # SKIP the segments are not files of /dev/shm"; exit 0; }

SHAPE="SELECT md5(x::text) FROM (SELECT count(DISTINCT s), min(s), max(s), count(*) FILTER (WHERE s > 'k8') FROM t1) x"
WANT=$(sql primary "SET sift.enable_agg = off" "SET sift.enable_col_scan = off" "SET max_parallel_workers_per_gather = 0" "$SHAPE")
# SETTING...: the rows, then held,hits,misses of a new backend
backend() {
	sql primary "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "$@" "$SHAPE" \
		"SELECT string_agg(value::text, ',' ORDER BY name) FROM sift.stats() WHERE name IN ('dict_shared_hits', 'dict_shared_held', 'dict_shared_misses')" | tr '\n' ' ' | sed 's/ $//'
}
segments() { local f n; for f in /dev/shm/PostgreSQL.*; do n=$(wc -c <"$f" 2>/dev/null | tr -d " ") && [[ -n $n ]] && echo "$n $f"; done | sort; true; }
sized() { segments | awk -v n="$1" '$1 == n' | wc -l; }
restart() { node_stop primary; node_conf primary "sift.col_dict_shared_cache_kb = $1"; node_start primary || bail "restart failed"; }

# off: the segments this backend maps (its own /proc/self/maps) are the same before and after it reads the dictionary, the caches of planning being attached first
maps() { echo "SELECT 'M:' || count(*) FROM regexp_split_to_table(pg_read_file('/proc/self/maps'), E'\n') l WHERE l LIKE '%/dev/shm/PostgreSQL.%'"; }
OUT=$(sql primary "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "EXPLAIN (COSTS OFF) $SHAPE" "$(maps)" "$SHAPE" "$(maps)" | grep -E '^(M:|[0-9a-f]{32}$)' | tr '\n' ' ' | sed 's/ $//')
A=$(backend)
B=$(backend)
check "0: two backends return the rows of the stock plan" test "$A|$B" == "$WANT 0,0,0|$WANT 0,0,0"
check "0: a backend maps no segment for the dictionary it reads" test "${OUT%% *}|${OUT##* }|$(cut -d' ' -f2 <<<"$OUT")" == "${OUT%% *}|${OUT%% *}|$WANT"

# 2000 kB: a ring of exactly that, kept by the first backend, read by the next
restart 2000
SEG=$(sized 2048000)
A=$(backend)
B=$(backend)
check "2000: the first backend keeps the dictionary, the second finds it, to the rows of the stock plan" test "${A% *}|${B% *}|$(( $(cut -d, -f3 <<<"${A##* }") > 0 ))|$(( $(cut -d, -f2 <<<"${B##* }") > 0 ))" == "$WANT|$WANT|1|1"
check "2000: the ring is one more segment, of 2000 kB" test "$(( $(sized 2048000) - SEG ))" == 1
OUT=$(sql primary "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "EXPLAIN (COSTS OFF) $SHAPE" "$(maps)" "$SHAPE" "$(maps)" | grep -E '^M:' | tr -d 'M:' | tr '\n' ' ')
check "2000: a backend maps the directory and the ring when it reads the dictionary" test "$(( ${OUT#* } - ${OUT%% *} ))" == 2
C=$(backend "SET sift.col_dict_shared_cache_kb = 8001")
check "2000: a backend that asks for 8001 kB reads the ring as it is" test "${C% *}|$(( $(cut -d, -f2 <<<"${C##* }") > 0 ))" == "$WANT|1"
check "2000: and no ring of 8001 kB is made" test "$(sized 8193024)" == 0
D=$(backend "SET sift.col_dict_shared_cache_kb = 0")
check "2000: a backend that sets 0 shares nothing, to the same rows" test "$D" == "$WANT 0,0,0"

# 250 kB: a record of half of it at most, the dictionary is more, so there is no ring
restart 250
SEG=$(sized 256000)
A=$(backend)
check "250: the dictionary is parsed by each backend, to the rows of the stock plan" test "$A" == "$WANT 0,0,0"
check "250: and no ring is made" test "$(( $(sized 256000) - SEG ))" == 0
finish
