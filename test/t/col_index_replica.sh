#!/usr/bin/env bash
# Physical streaming replica (pg_basebackup): inserts, updates, deletes, VACUUM, promotion and merge of a sift_col index on the primary
# replay to an index that verifies on the hot standby and answers the column source exactly as the primary does. Readers on the standby
# run all the while (with hot_standby_feedback on none may fail). The weight leaf holds numbers among strings, booleans, objects, arrays and JSON
# nulls (overflow values with their JSON types in the chunks) and some documents lack it: jsonb_typeof filters (=, <>, 'null', IS NULL), a group key and FILTERs return the same on the standby, the primary and the heap.
# The standby is then promoted and takes writes.
. "$(dirname "$0")/lib.bash"

SECS=$(knob SIFT_COL_REPLICA_SECS 15 3)
plan 15
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = $(knob SIFT_WAL_KEEP 1GB 256MB)
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE FUNCTION weight_of(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS \$\$ SELECT CASE i % 17 WHEN 0 THEN '\"heavy\"' WHEN 1 THEN 'true' WHEN 2 THEN '{\"kg\": 1}'
		WHEN 3 THEN '[1]' WHEN 4 THEN 'null' ELSE to_jsonb((i % 40) / 4.0::float8) END::jsonb \$\$" \
	"CREATE FUNCTION weight_doc(i int) RETURNS jsonb LANGUAGE sql IMMUTABLE AS \$\$ SELECT CASE WHEN i % 23 = 0 THEN '{}'::jsonb ELSE jsonb_build_object('weight', weight_of(i)) END \$\$" \
	"CREATE FUNCTION typeof_same() RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE q text; a text; b text;
		BEGIN
			SET LOCAL seq_page_cost = 10000;
			FOREACH q IN ARRAY ARRAY[
				'SELECT (count(*), sum((doc->>''weight'')::float8))::text FROM orders WHERE jsonb_typeof(doc->''weight'') = ''number''',
				'SELECT count(*)::text FROM orders WHERE jsonb_typeof(doc->''weight'') <> ''number''',
				'SELECT count(*)::text FROM orders WHERE jsonb_typeof(doc->''weight'') = ''null''',
				'SELECT count(*)::text FROM orders WHERE jsonb_typeof(doc->''weight'') IS NULL',
				'SELECT string_agg(coalesce(t, ''-'') || '':'' || n, '','' ORDER BY t NULLS FIRST) FROM (SELECT jsonb_typeof(doc->''weight'') AS t, count(*) AS n FROM orders GROUP BY 1) x',
				'SELECT (count(*) FILTER (WHERE jsonb_typeof(doc->''weight'') = ''string'' OR tenant_id < 5), count(*) FILTER (WHERE jsonb_typeof(doc->''weight'') IS NULL))::text FROM orders']
			LOOP
				SET LOCAL sift.enable_agg = on;
				EXECUTE q INTO a;
				SET LOCAL sift.enable_agg = off;
				EXECUTE q INTO b;
				IF a IS DISTINCT FROM b THEN RETURN false; END IF;
			END LOOP;
			RETURN true;
		END \$\$" \
	"CREATE FUNCTION gone_same() RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE q text := \$q\$SELECT coalesce(string_agg(status || ':' || n || ':' || s, ',' ORDER BY status), '') FROM (SELECT status, count(*) AS n, sum(qty) AS s FROM orders WHERE gone IS NULL GROUP BY 1) x\$q\$; a text; b text;
		BEGIN
			SET LOCAL seq_page_cost = 10000;
			SET LOCAL sift.enable_agg = on;
			EXECUTE q INTO a;
			SET LOCAL sift.enable_agg = off;
			EXECUTE q INTO b;
			RETURN a IS NOT DISTINCT FROM b;
		END \$\$" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, tenant_id int, status text, qty bigint, gone bigint, doc jsonb)" \
	"INSERT INTO orders (tenant_id, status, qty, gone, doc) SELECT i % 50, (ARRAY['new','paid','void'])[1 + i % 3], i, CASE WHEN i % 500 = 0 THEN i END,
		jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) || weight_doc(i) FROM generate_series(1, 20000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (tenant_id, status, qty, gone, doc) WITH (write_buffer_limit = 64, paths = 'doc.items[]{category text, qty int4}, doc.weight float8',
	preagg = 'cube(tenant_id: qty; status: qty) rollup(doc.items[].category: doc.items[].qty)', hll_columns = 'tenant_id, status, qty')" \
	>/dev/null || bail "setup failed"

node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

P=$(dir primary)
cat >"$P/ins.sql" <<'EOF'
INSERT INTO orders (tenant_id, status, qty, doc) SELECT (random() * 50)::int, 'paid', i,
	jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) || weight_doc((random() * 1000)::int) FROM generate_series(1, 20) i;
EOF
cat >"$P/upd.sql" <<'EOF'
\set id random(1, 100000)
UPDATE orders SET tenant_id = coalesce(tenant_id, 0) + 1 WHERE id BETWEEN :id AND :id + 200;
UPDATE orders SET gone = id WHERE id = :id;
DELETE FROM orders WHERE id BETWEEN :id + 1000 AND :id + 1020;
EOF
echo "VACUUM orders;" >"$P/vac.sql"
cat >"$P/maint.sql" <<'EOF'
SELECT sift.col_promote('o_col');
SELECT sift.col_merge('o_col', random() < 0.3);
EOF
cat >"$P/read.sql" <<'EOF'
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid');
SELECT typeof_same();
SELECT gone_same();
COMMIT;
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
	|| ' ' || (SELECT count(*) FROM orders) || ' ' || (SELECT count(*) FROM orders WHERE tenant_id < 10 AND status = 'paid')"
check "standby answers the column source as the primary does" test "$(sql standby "$CHECKS")" == "$(sql primary "$CHECKS")"
check "the standby computes the groups of the elements from the element cubes as the primary does" \
	test "$(elem_groups standby on)" == "$(elem_groups primary off)"
sleep 2   # the standby's feedback (wal_receiver_status_interval) no longer holds the horizon of its finished readers
sql primary "VACUUM (disable_page_skipping) orders" "SELECT sift.col_merge('o_col', true)" >/dev/null
catchup primary standby
GONE="SELECT status, count(*), sum(qty) FROM orders WHERE gone IS NULL GROUP BY 1 ORDER BY 1"
check "the standby answers the soft-delete filter as the primary and the heap do" \
	test "$(sql standby "SET seq_page_cost = 10000" "$GONE")" == "$(sql primary "SET sift.enable_agg = off" "$GONE")" -a "$(sql standby "SELECT gone_same()")" == t
check "the standby answers units of the merged segment from the exceptions" \
	grep -qE "Units Net Of Exceptions: [1-9]" <<<"$(sql standby "SET seq_page_cost = 10000" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) $GONE")"
APPROX="SELECT sift.approx_count_distinct(qty) || ' ' || sift.approx_count_distinct(status) || ' ' || sift.approx_count_distinct(tenant_id) FROM orders"
check "the standby merges the sketches of its units to the number the primary's heap gives" \
	test "$(sql standby "SET sift.agg_force = on" "$APPROX")" == "$(sql primary "SET sift.enable_agg = off" "$APPROX")"
check "the index verifies on the standby" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
TYPED="SELECT count(*) || ' ' || sum((doc->>'weight')::float8) FROM orders WHERE jsonb_typeof(doc->'weight') = 'number'"
check "a type filter plans as an aggregate scan on the standby" \
	test "$(sql standby "SET seq_page_cost = 10000" "EXPLAIN (COSTS OFF) $TYPED" | grep -c SiftAgg)" == 1
check "the standby's type filter returns what its heap returns" test "$(sql standby "SELECT typeof_same()")" == t
check "and what the primary's returns" \
	test "$(sql standby "SET seq_page_cost = 10000" "$TYPED")" == "$(sql primary "SET seq_page_cost = 10000" "$TYPED")"
SEGS="SELECT string_agg(gen || ':' || kind || ':' || coalesce(rows, 0), ' ' ORDER BY gen) FROM sift.col_segments('o_col') WHERE kind = 'immutable'"
check "standby immutable segments equal the primary's" test "$(sql standby "$SEGS")" == "$(sql primary "$SEGS")"

node_promote standby
for _ in $(seq 100); do [[ $(sql standby "SELECT pg_is_in_recovery()") == f ]] && break; sleep 0.1; done
sql standby "INSERT INTO orders (tenant_id, status, qty, doc) SELECT 1, 'paid', i, '{\"items\": [{\"category\": \"c1\", \"qty\": 1}]}' FROM generate_series(1, 500) i" \
	"SELECT sift.col_promote('o_col')" "SELECT sift.col_merge('o_col', true)" "VACUUM orders" >/dev/null
check "the promoted standby takes writes and its index verifies" test -z "$(sql standby "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "the promoted standby's index holds what its heap holds" \
	test "$(sql standby "SELECT (SELECT count(*) FROM sift._test_col_tids('o_col', '')) = (SELECT count(*) FROM orders) AND typeof_same()")" == t
finish
