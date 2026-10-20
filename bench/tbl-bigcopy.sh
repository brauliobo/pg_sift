#!/usr/bin/env bash
# One transaction loads a lot of rows into a sift_table with COPY: the write buffers of the running transaction become column groups that
# carry their writer, so that the limit of the directory does not end the load. Run once under the perf lock (it is a load, not a gate:
# the regress test tbl_bigtx lowers the limit to cover the same paths in seconds). The load, the commit and VACUUM are timed, the peak
# memory of the backend is sampled, and the table is checked: count, sums, an index probe, and the answers of a plain scan against the
# aggregate scan, before and after VACUUM.
#
#   bench/tbl-bigcopy.sh [ROWS]      (default 5000000; the cluster of scripts/pgtest.sh must be running, with pg_sift installed:
#                                     SIFT_TMP=... SIFT_PORT=... scripts/pgtest.sh install && flock -o /tmp/sift-perf.lock scripts/pgtest.sh start)
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=${1:-5000000}
PG="$REPO/scripts/pgtest.sh"
q() { "$PG" psql -X -qAt "$@"; }
exec 9>${PERF_LOCK:-/tmp/sift-perf.lock}
flock -n 9 || { echo "tbl-bigcopy: the perf lock is held, try later" >&2; exit 2; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
check() { if [[ $2 == "$3" ]]; then echo "ok   $1: $2"; else echo "FAIL $1: got $2, want $3"; fail=1; fi; }

q -c "CREATE EXTENSION IF NOT EXISTS pg_sift" -c "DROP TABLE IF EXISTS big_orders" >/dev/null
q -c "CREATE TABLE big_orders (id bigint PRIMARY KEY, customer_id int NOT NULL, country text NOT NULL, status text NOT NULL, amount numeric(10,2) NOT NULL,
	quantity smallint NOT NULL, created_at timestamp NOT NULL, title text NOT NULL) USING sift_table WITH (autovacuum_enabled = false)" >/dev/null

cat >"$WORK/gen.sh" <<GEN
seq 1 $ROWS | awk 'BEGIN { split("BR US DE FR JP IN", c, " "); split("new paid shipped returned", s, " ") }
	{ printf "%d,%d,%s,%s,%.2f,%d,2026-01-%02d %02d:00:00,order %d\\n", \$1, \$1 % 100000, c[1 + \$1 % 6], s[1 + \$1 % 4], (\$1 % 99999) / 100, 1 + \$1 % 9, 1 + \$1 % 28, \$1 % 24, \$1 }'
GEN
t0=$(date +%s.%N)
"$PG" psql -X -qAt -c "BEGIN" -c "SELECT pg_backend_pid()" -c "\\copy big_orders FROM PROGRAM 'bash $WORK/gen.sh' CSV" -c "SELECT count(*) FROM big_orders" -c "COMMIT" >"$WORK/out" 2>&1 &
PSQL=$!
peak=0
while kill -0 $PSQL 2>/dev/null; do
	pid=$(head -1 "$WORK/out" 2>/dev/null)
	if [[ $pid =~ ^[0-9]+$ ]]; then
		hwm=$(awk '/^VmHWM/ {print $2}' "/proc/$pid/status" 2>/dev/null)
		[[ -n ${hwm:-} ]] && (( hwm > peak )) && peak=$hwm
	fi
	sleep 0.2
done
wait $PSQL || { cat "$WORK/out"; exit 1; }
echo "one transaction, $ROWS rows: $(echo "$(date +%s.%N) - $t0" | bc) s, backend peak memory $((peak / 1024)) MB"
check "count seen by the writer" "$(sed -n 2p "$WORK/out")" "$ROWS"

want="$ROWS:$((ROWS * (ROWS + 1) / 2))"
sums="SELECT count(*) || ':' || sum(id) FROM big_orders"
agg="SELECT country || status || ':' || count(*) || ':' || sum(amount) || ':' || sum(quantity) FROM big_orders GROUP BY country, status ORDER BY 1"
for phase in committed vacuumed; do
	[[ $phase == vacuumed ]] && { t0=$(date +%s.%N); q -c "VACUUM big_orders" >/dev/null; echo "VACUUM: $(echo "$(date +%s.%N) - $t0" | bc) s"; }
	check "$phase: count and sum" "$(q -c "$sums")" "$want"
	check "$phase: index probe" "$(q -c "SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM big_orders WHERE id BETWEEN 1000 AND 1999")" 1000
	plain=$(q -c "SET sift.enable_agg = off; $agg" | md5sum)
	check "$phase: aggregate scan = plain scan" "$(q -c "SET sift.enable_agg = on; $agg" | md5sum)" "$plain"
	check "$phase: no row is wrong" "$(q -c "SELECT count(*) FROM big_orders WHERE title <> 'order ' || id OR customer_id <> id % 100000")" 0
done
q -c "SELECT * FROM sift.table_space('big_orders')" -c "SELECT kind, count(*), sum(rows) FROM sift.table_segments('big_orders') GROUP BY 1"
q -c "DROP TABLE big_orders" >/dev/null
(( fail )) && { echo "tbl-bigcopy: FAILED"; exit 1; }
echo "tbl-bigcopy: ok"
