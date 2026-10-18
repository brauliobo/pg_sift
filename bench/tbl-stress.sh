#!/usr/bin/env bash
# A sift_table with a sift BM25 index and a btree key, under updates at a high rate while VACUUM, the index merges and the index checks run
# side by side. Any "corrupted" error of the clients or the server log fails the run; at the end the indexes are verified
# and the rows read through every path are compared.
#
#   bench/tbl-stress.sh [SECONDS]      (default 1200; the cluster of scripts/pgtest.sh must be running, with pg_sift installed:
#                                       SIFT_TMP=... SIFT_PORT=... scripts/pgtest.sh install && scripts/pgtest.sh start)
#   TBL_ROWS=100000 TBL_CLIENTS=16     table size, pgbench clients
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-1200}
ROWS=${TBL_ROWS:-100000}
CLIENTS=${TBL_CLIENTS:-16}
PG="$REPO/scripts/pgtest.sh"
q() { "$PG" psql -X -qAt "$@"; }
eval "$("$PG" env)"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

q -c "CREATE EXTENSION IF NOT EXISTS pg_sift" -c "CREATE EXTENSION IF NOT EXISTS amcheck" >/dev/null
q -c "DROP TABLE IF EXISTS orders"
q -c "CREATE TABLE orders (id int PRIMARY KEY, store int, qty int, price float8, note text) USING sift_table"
q -c "INSERT INTO orders SELECT g, g % 200, g % 50, (g % 1000) / 10.0, 'order ' || g || ' ' || (ARRAY['express','standard','late','refund'])[1 + g % 4] FROM generate_series(1, $ROWS) g"
q -c "CREATE INDEX orders_bm25 ON orders USING sift (note)"
q -c "VACUUM ANALYZE orders"

cat >"$WORK/upd.sql" <<EOF
\set id random(1, $ROWS)
UPDATE orders SET qty = qty + 1, price = price + 0.5, note = 'order ' || :id || ' rev ' || random(1, 1000) WHERE id = :id;
EOF
cat >"$WORK/ins.sql" <<EOF
\set id random($ROWS + 1, $ROWS * 2)
INSERT INTO orders VALUES (:id, :id % 200, 1, 1.5, 'order ' || :id || ' new') ON CONFLICT (id) DO UPDATE SET qty = orders.qty + 1;
EOF
cat >"$WORK/del.sql" <<EOF
\set id random($ROWS + 1, $ROWS * 2)
DELETE FROM orders WHERE id = :id;
EOF
echo "VACUUM orders;" >"$WORK/vac.sql"
cat >"$WORK/maint.sql" <<EOF
SELECT sift.promote('orders_bm25');
SELECT sift.merge('orders_bm25');
EOF
cat >"$WORK/read.sql" <<EOF
SELECT store, count(*), sum(qty) FROM orders GROUP BY store ORDER BY 1 LIMIT 3;
SELECT count(*) FROM orders WHERE note @@> 'express';
EOF
echo "SELECT sift.verify('orders_bm25');" >"$WORK/verify.sql"

# rounds of 60 s: pgbench ends a client at its first error, a new round starts them all again
: >"$WORK/out"
for ((left = SECS; left > 0; left -= 60)); do
	pgbench -n -h "$PGHOST" -p "$PGPORT" -c "$CLIENTS" -j "$CLIENTS" -T "$((left < 60 ? left : 60))" \
		-f "$WORK/upd.sql@60" -f "$WORK/ins.sql@8" -f "$WORK/del.sql@4" -f "$WORK/vac.sql@1" -f "$WORK/maint.sql@2" -f "$WORK/read.sql@6" \
		-f "$WORK/verify.sql@1" "${PGDATABASE:-sift}" >>"$WORK/out" 2>&1
done
echo "transactions per second, by round of 60 s: $(awk '/^tps = /{printf "%d ", $3}' "$WORK/out")"
grep "pgbench: error" "$WORK/out" | sed 's/client [0-9]* script [0-9]* //' | sort | uniq -c | sort -rn | head -8

rc=0
if grep -q "corrupted" "$WORK/out"; then echo "FAIL: a client saw a corruption error:"; grep -m5 "corrupted" "$WORK/out"; rc=1; fi
if grep -q "corrupted" "${SIFT_TMP:-/tmp/sift-arch}/postgres.log" 2>/dev/null; then echo "FAIL: the server log has corruption errors:"; grep -m5 "corrupted" "${SIFT_TMP:-/tmp/sift-arch}/postgres.log"; rc=1; fi
q -c "SELECT sift.verify('orders_bm25', true)" >/dev/null || { echo "FAIL: sift.verify"; rc=1; }
q -c "SELECT bt_index_check('orders_pkey', true)" >/dev/null || { echo "FAIL: bt_index_check"; rc=1; }
seq=$(q -c "SET sift.enable_agg = off; SET enable_indexscan = off; SET enable_bitmapscan = off; SELECT count(*) || ':' || sum(qty) FROM orders" | tail -1)
agg=$(q -c "SET cpu_tuple_cost = 0.5; SELECT count(*) || ':' || sum(qty) FROM orders" | tail -1)
echo "rows through the sequential scan $seq, through SiftAgg $agg"
[[ $seq == "$agg" ]] || { echo "FAIL: the plans disagree"; rc=1; }
((rc)) || echo "PASS: ${SECS}s of updates, no corruption"
exit "$rc"
