#!/usr/bin/env bash
# sift_table against heap on the scripts/pgtest.sh cluster: the same orders table loaded and queried both ways, one timing per line
# (milliseconds, best of 3 for reads; the writes run once), and the relation sizes.
#
#   bench/tbl.sh [ROWS]        (default 1000000; TBL_BATCH rows per load transaction, default 100000; start the cluster first: scripts/pgtest.sh install && scripts/pgtest.sh start)
#   TBL_IC=1 bench/tbl.sh ROWS the same with the user-space instructions (millions, perf stat on the postmaster and the processes it forks,
#                              one session's cost subtracted, no parallel workers) instead of milliseconds: stable on a busy host
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROWS=${1:-1000000}
q() { "$REPO/scripts/pgtest.sh" psql -X -qAt "$@"; }
UNIT=ms
if [[ -n ${TBL_IC:-} ]]; then
	UNIT=Minstr
	export PGOPTIONS='-c max_parallel_workers_per_gather=0'
	PM=$(head -1 "${SIFT_TMP:-/tmp/sift-arch}/data/postmaster.pid")
	ms() { # SQL -> millions of user instructions of the server processes
		local out
		out=$(mktemp)
		perf stat -x, -e instructions:u -p "$PM" -o "$out" &
		local pp=$!
		sleep 0.2
		q -c "$1" >/dev/null
		kill -INT "$pp"
		wait "$pp" || true
		awk -F, '/instructions/ { printf "%d", $1 / 1e6 }' "$out"
		rm -f "$out"
	}
else
	ms() { # SQL -> elapsed milliseconds
		local t0 t1
		t0=$(date +%s%N)
		q -c "$1" >/dev/null
		t1=$(date +%s%N)
		echo $(((t1 - t0) / 1000000))
	}
fi
best() { local b=999999999 v; for _ in 1 2 3; do v=$(ms "$1"); ((v < b)) && b=$v; done; echo "$b"; }
row() { printf '%-34s heap %8s %s   sift_table %8s %s   ratio %s\n' "$1" "$2" "$UNIT" "$3" "$UNIT" "$(awk -v a="$2" -v b="$3" 'BEGIN { printf "%.2f", (a > 0 ? b / a : 0) }')"; }

q -c "CREATE EXTENSION IF NOT EXISTS pg_sift" >/dev/null
for t in b_heap b_tam; do q -c "DROP TABLE IF EXISTS $t"; done
q -c "CREATE TABLE b_heap (id bigint, tenant_id int, placed_at timestamptz, status text, total numeric(12,2), items jsonb)"
q -c "CREATE TABLE b_tam  (id bigint, tenant_id int, placed_at timestamptz, status text, total numeric(12,2), items jsonb) USING sift_table"

# a load of many rows is many transactions (BATCH rows each): a transaction's rows are sealed into columns only once it ends, and one
# transaction can write about 500 MB of row format before the directory is full (sift_table "has 126 segments")
BATCH=${TBL_BATCH:-100000}
load_range() { echo "INSERT INTO $1 SELECT g, g % 500, timestamptz '2025-01-01' + g * interval '1 minute', (ARRAY['new','paid','shipped','done'])[1 + g % 4],
	(g % 10000) / 100.0, jsonb_build_array(jsonb_build_object('sku', g % 1000, 'qty', 1 + g % 5, 'price', (g % 90) + 0.5)) FROM generate_series($2, $3) g"; }
load() { # TABLE -> elapsed ms of the whole load
	local t0 t1 lo
	t0=$(date +%s%N)
	for ((lo = 1; lo <= ROWS; lo += BATCH)); do q -c "$(load_range "$1" "$lo" "$((lo + BATCH - 1 > ROWS ? ROWS : lo + BATCH - 1))")" >/dev/null; done
	t1=$(date +%s%N)
	echo $(((t1 - t0) / 1000000))
}
th=$(load b_heap)
tt=$(load b_tam)
row "bulk INSERT SELECT $ROWS rows" "$th" "$tt"
if [[ -z ${TBL_IC:-} ]]; then
	printf '%-34s heap %8d rows/s   sift_table %8d rows/s\n' "load rate ($BATCH rows per transaction)" "$((ROWS * 1000 / (th > 0 ? th : 1)))" "$((ROWS * 1000 / (tt > 0 ? tt : 1)))"
fi
row "CREATE INDEX btree (id)" "$(ms 'CREATE INDEX b_heap_id ON b_heap (id)')" "$(ms 'CREATE INDEX b_tam_id ON b_tam (id)')"
q -c "VACUUM ANALYZE b_heap" -c "VACUUM ANALYZE b_tam" >/dev/null

both() { # LABEL SQL-with-@T
	row "$1" "$(best "${2//@T/b_heap}")" "$(best "${2//@T/b_tam}")"
}
both "count(*)" "SELECT count(*) FROM @T"
both "sum(total) group by tenant_id" "SELECT tenant_id, sum(total) FROM @T GROUP BY 1"
both "filter status=paid, sum" "SELECT sum(total) FROM @T WHERE status = 'paid'"
both "jsonb extract, sum" "SELECT sum((items->0->>'qty')::int) FROM @T"
both "btree point lookup x1000" "SELECT count(*) FROM generate_series(1, 1000) g JOIN @T t ON t.id = g * 997 % $ROWS + 1"
both "btree range 10000 rows" "SELECT sum(total) FROM @T WHERE id BETWEEN 1000 AND 11000"

row "UPDATE 10% non-key" "$(ms 'UPDATE b_heap SET total = total + 1 WHERE id % 10 = 0')" "$(ms 'UPDATE b_tam SET total = total + 1 WHERE id % 10 = 0')"
row "DELETE 5%" "$(ms 'DELETE FROM b_heap WHERE id % 20 = 1')" "$(ms 'DELETE FROM b_tam WHERE id % 20 = 1')"
both "count(*) after writes" "SELECT count(*) FROM @T"
row "VACUUM" "$(ms 'VACUUM b_heap')" "$(ms 'VACUUM b_tam')"
both "count(*) after vacuum" "SELECT count(*) FROM @T"
row "VACUUM FULL" "$(ms 'VACUUM (FULL) b_heap')" "$(ms 'VACUUM (FULL) b_tam')"

echo "sizes (MB, table / indexes):"
q -c "SELECT 'heap       ' || pg_relation_size('b_heap') / 1048576 || ' / ' || pg_indexes_size('b_heap') / 1048576" \
	-c "SELECT 'sift_table ' || pg_relation_size('b_tam') / 1048576 || ' / ' || pg_indexes_size('b_tam') / 1048576"
q -c "DROP TABLE b_heap, b_tam" >/dev/null
