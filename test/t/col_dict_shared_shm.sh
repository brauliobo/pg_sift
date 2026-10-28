#!/usr/bin/env bash
# The parsed dictionaries that backends share (col/dictshm.c) in a /dev/shm of a container (Docker's default is 64 MB; where /dev/shm is bigger than 1 GB the
# test uses sift.test_col_dict_shared = 3, which makes the table fail as a full one does): the table takes a quarter of /dev/shm at most, a second backend
# copies what the first parsed to the same rows as the stock plan (also in a parallel query), a ring too small for the dictionaries asked stops writing instead
# of churning, and a /dev/shm with no room for the table costs no error and no wrong row: the backend goes without it, as before.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 9
node_init primary 2
node_start primary || bail "primary start failed"
TABLES=14
{
	echo "CREATE EXTENSION pg_sift;"
	for t in $(seq $TABLES); do
		echo "CREATE TABLE t$t AS SELECT g AS id, 'k$t' || md5(g::text) AS s FROM generate_series(1, 150000) g;"
		echo "CREATE INDEX t${t}_col ON t$t USING sift_col (id, s) WITH (rollup_dims = 'off');"
		echo "VACUUM (FREEZE, ANALYZE) t$t;"
	done
} | sql primary >/dev/null || bail "setup failed"

# the rows of a table, as an md5, from the stock plan ($2 = off) or the column scan ($2 = on); then its counters (held,hits,misses, by name)
shape() { echo "SELECT md5(x::text) FROM (SELECT count(DISTINCT s), min(s), max(s), count(*) FILTER (WHERE s > 'k${1}8') FROM t$1) x"; }
expect() { sql primary "SET sift.enable_agg = off" "SET sift.enable_col_scan = off" "SET max_parallel_workers_per_gather = 0" "$(shape "$1")"; }
counters() { echo "SELECT string_agg(value::text, ',' ORDER BY name) FROM sift.stats() WHERE name IN ('dict_shared_hits', 'dict_shared_held', 'dict_shared_misses')"; }
backend() { # SETTING... -- tables: the md5 of each table, then hits,held,misses
	local pre=() t
	while [[ $1 != -- ]]; do pre+=("$1"); shift; done
	shift
	sql primary "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "${pre[@]}" "$(for t; do shape "$t"; echo ";"; done)" "$(counters)" 2>&1 | tr '\n' ' ' | sed 's/ $//'
}
declare -A WANT
for t in $(seq $TABLES); do WANT[$t]=$(expect "$t"); done
nth() { echo "$1" | cut -d' ' -f"$2"; }
ctr() { echo "$1" | cut -d' ' -f"$2" | cut -d, -f$(case $3 in held) echo 1;; hits) echo 2;; *) echo 3;; esac); }

# the table is made again at a start of the server
node_stop primary
node_start primary || bail "restart failed"
A=$(backend "SET sift.enable_agg = on" -- 1)
check "the first backend parses the dictionaries, to the rows of the stock plan, and offers them" test "$(nth "$A" 1)|$(ctr "$A" 2 held)|$(ctr "$A" 2 hits)|$(( $(ctr "$A" 2 misses) > 0 ))" == "${WANT[1]}|0|0|1"
B=$(backend "SET sift.enable_agg = on" -- 1)
check "the second copies them, to the same rows" test "$(nth "$B" 1)|$(( $(ctr "$B" 2 hits) > 0 ))|$(ctr "$B" 2 misses)" == "${WANT[1]}|1|0"

# a parallel query: workers are backends of their own, the leader's table or none, the same rows
P=$(sql primary "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 2" "SET parallel_setup_cost = 0" "SET parallel_tuple_cost = 0" \
	"SET min_parallel_table_scan_size = 0" "$(shape 1)" "SELECT value > 0 FROM sift.stats() WHERE name = 'workers_launched'" | tr '\n' ' ')
check "a parallel query, its workers joined to the table, returns the same rows" test "$P" == "${WANT[1]} t "

# the segment of the table in a /dev/shm of 64 MB: a quarter of it at most
SHM_KB=$(df -Pk /dev/shm | awk 'NR == 2 {print $2}')
if ((SHM_KB <= 1048576)); then
	BIG=$(find /dev/shm -maxdepth 1 -name 'PostgreSQL.*' -size +$((SHM_KB / 4 + 1024))k | wc -l)
	check "the table takes no more than a quarter of /dev/shm" test "$BIG" == 0
else
	skip 1 "/dev/shm is $((SHM_KB / 1024)) MB: the cap of a small one is not reached"
fi

# fourteen dictionaries of about 6 MB through a ring that holds fewer of them: the guard stops the writes, no wrong row on the way
HELD=0 WRONG=0
for r in $(seq 16); do
	R=$(backend "SET sift.enable_agg = on" -- $(seq $TABLES))
	HELD=$((HELD + $(ctr "$R" $((TABLES + 1)) held)))
	for t in $(seq $TABLES); do [[ $(nth "$R" "$t") == "${WANT[$t]}" ]] || WRONG=$((WRONG + 1)); done
done
check "a ring too small for the dictionaries asked stops writing" test "$HELD" -gt 0
check "and every table returned the rows of the stock plan" test "$WRONG" == 0

# no room for the table: the registry cannot make the segment, the backend logs it and goes without
if ((SHM_KB <= 1048576)); then
	node_stop primary
	FREE_KB=$(df -Pk /dev/shm | awk 'NR == 2 {print $4}')
	fallocate -l "$(( (FREE_KB - 2048) * 1024 ))" /dev/shm/col_dict_shared_ballast || bail "no ballast"
	trap 'rm -f /dev/shm/col_dict_shared_ballast; cleanup' EXIT
	MODE=
else
	node_stop primary
	MODE="SET sift.test_col_dict_shared = 3"
fi
node_start primary || bail "restart failed"
N=$(backend "SET sift.enable_agg = on" ${MODE:+"$MODE"} -- 1 2)
check "a /dev/shm with no room for the table: the rows are those of the stock plan" test "$(nth "$N" 1)|$(nth "$N" 2)" == "${WANT[1]}|${WANT[2]}"
check "and nothing was shared" test "$(nth "$N" 3)" == "0,0,0"
check "the backend logged it once and did not fail" grep -q 'the parsed dictionaries of the column indexes are not shared with other backends' "$(dir primary)/postgres.log"
rm -f /dev/shm/col_dict_shared_ballast
finish
