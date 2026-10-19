#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup): inserts, updates, deletes, VACUUM, promotion and merge of a sift_col index on the primary
# replay to an index that verifies on the hot standby and answers the column source exactly as the primary does. Readers on the standby
# run all the while (with hot_standby_feedback on none may fail). The standby is then promoted and takes writes.
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 8
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, store_id int, status text, qty bigint, doc jsonb)" \
	"INSERT INTO orders (store_id, status, qty, doc) SELECT i % 50, (ARRAY['new','paid','void'])[1 + i % 3], i,
		jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) FROM generate_series(1, 20000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (store_id, status, qty, doc) WITH (write_buffer_limit = 64, paths = 'doc.items[]{category text, qty int4}')" \
	>/dev/null || bail "setup failed"

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

P=$(dir primary)
cat >"$P/ins.sql" <<'EOF'
INSERT INTO orders (store_id, status, qty, doc) SELECT (random() * 50)::int, 'paid', i,
	jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) FROM generate_series(1, 20) i;
EOF
cat >"$P/upd.sql" <<'EOF'
\set id random(1, 100000)
UPDATE orders SET store_id = coalesce(store_id, 0) + 1 WHERE id BETWEEN :id AND :id + 200;
DELETE FROM orders WHERE id BETWEEN :id + 1000 AND :id + 1020;
EOF
echo "VACUUM orders;" >"$P/vac.sql"
cat >"$P/maint.sql" <<'EOF'
SELECT sift.col_promote('o_col');
SELECT sift.col_merge('o_col', random() < 0.3);
EOF
cat >"$P/read.sql" <<'EOF'
SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid');
EOF
pgbench_bg primary 6 "$SECS" -f "$P/ins.sql@40" -f "$P/upd.sql@10" -f "$P/vac.sql@1" -f "$P/maint.sql@2"
PRIMARY_PID=$PGBENCH_PID
pgbench -n -h "$(dir standby)/sock" -p "$(port standby)" -U postgres -c 2 -j 2 -T "$SECS" -f "$P/read.sql" postgres >"$P/reader.log" 2>&1 &
READER=$!
wait "$PRIMARY_PID"
wait "$READER"
check "churn on the primary had no failed transaction" grep -q "number of failed transactions: 0 " "$P/pgbench.log"
check "readers on the standby had no failed transaction" grep -q "number of failed transactions: 0 " "$P/reader.log"
catchup primary standby

CHECKS="SELECT (SELECT count(*) FROM sift._test_col_tids('o_col', '')) || ' ' || (SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid'))
	|| ' ' || (SELECT count(*) FROM orders) || ' ' || (SELECT count(*) FROM orders WHERE store_id < 10 AND status = 'paid')"
check "standby answers the column source as the primary does" test "$(sql standby "$CHECKS")" == "$(sql primary "$CHECKS")"
check "the index verifies on the standby" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
SEGS="SELECT string_agg(gen || ':' || kind || ':' || coalesce(rows, 0), ' ' ORDER BY gen) FROM sift.col_segments('o_col') WHERE kind = 'immutable'"
check "standby immutable segments equal the primary's" test "$(sql standby "$SEGS")" == "$(sql primary "$SEGS")"

node_promote standby
for _ in $(seq 100); do [[ $(sql standby "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql standby "INSERT INTO orders (store_id, status, qty, doc) SELECT 1, 'paid', i, '{\"items\": [{\"category\": \"c1\", \"qty\": 1}]}' FROM generate_series(1, 500) i" \
	"SELECT sift.col_promote('o_col')" "SELECT sift.col_merge('o_col', true)" "VACUUM orders" >/dev/null
check "the promoted standby takes writes and its index verifies" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "the promoted standby's index holds what its heap holds" \
	test "$(sql standby "SELECT (SELECT count(*) FROM sift._test_col_tids('o_col', '')) = (SELECT count(*) FROM orders)")" == t
finish
