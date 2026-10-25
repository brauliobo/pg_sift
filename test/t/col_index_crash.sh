#!/usr/bin/env bash
# kill -9 of the postmaster and all backends in the middle of concurrent inserts, updates, deletes, VACUUM, promotion and merge of a
# sift_col index, then crash recovery and checks: the store and every segment verify, the index holds exactly the visible heap tuples
# with their values (heapallindexed), the column source returns what the heap returns, the groups of the elements of the documents come out
# of the element cubes as the normal plan computes them, and the space stays bounded (the sweep reclaims
# what the crash orphaned). The weight leaf holds numbers among strings, booleans, objects, arrays and JSON nulls (overflow values with their
# JSON types in the chunks) and some documents lack it: jsonb_typeof filters (=, <>, 'null', IS NULL), a group key and FILTERs of the column source
# return what the heap returns. SIFT_COL_CRASH_ITERS (default 40, 4 quick).
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_col.bash"

ITERS=$(knob SIFT_COL_CRASH_ITERS 40 4)
MAX_MB=$(knob SIFT_CRASH_MAX_MB 4096 1024)
plan $((ITERS + 8))
node_init crash 1 "checkpoint_timeout = 30s
max_wal_size = 256MB
autovacuum = off"
node_start crash || bail "start failed"
sql crash "CREATE EXTENSION pg_sift_test CASCADE" \
	"${COL_WEIGHT_FUNCS[@]}" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, tenant_id int, status text, qty bigint, price float8, gone bigint, doc jsonb)" \
	"INSERT INTO orders (tenant_id, status, qty, price, gone, doc) SELECT i % 50, (ARRAY['new','paid','void'])[1 + i % 3], i, i / 4.0, CASE WHEN i % 500 = 0 THEN i END,
		jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) || weight_doc(i) FROM generate_series(1, 20000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (tenant_id, status, qty, price, gone, doc)
		WITH (write_buffer_limit = 64, paths = 'doc.items[]{category text, qty int4}, doc.weight float8',
			preagg = 'cube(tenant_id: qty, price; status: qty) rollup(doc.items[].category: doc.items[].qty)')" >/dev/null || bail "setup failed"
ok "setup"
check "the type filter plans as an aggregate scan" test "$(sql crash "SET seq_page_cost = 10000" "EXPLAIN (COSTS OFF) SELECT count(*) FROM orders WHERE jsonb_typeof(doc->'weight') = 'number'" | grep -c SiftAgg)" == 1

D=$(dir crash)
cat >"$D/ins.sql" <<'EOF'
INSERT INTO orders (tenant_id, status, qty, price, doc) SELECT (random() * 50)::int, 'paid', i, i / 2.0,
	jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) || weight_doc((random() * 1000)::int) FROM generate_series(1, 20) i;
EOF
cat >"$D/upd.sql" <<'EOF'
\set id random(1, 100000)
UPDATE orders SET tenant_id = coalesce(tenant_id, 0) + 1 WHERE id BETWEEN :id AND :id + 200;
UPDATE orders SET gone = id WHERE id = :id;
EOF
cat >"$D/del.sql" <<'EOF'
\set id random(1, 100000)
DELETE FROM orders WHERE id BETWEEN :id AND :id + 30;
EOF
echo "VACUUM orders;" >"$D/vac.sql"
cat >"$D/maint.sql" <<'EOF'
SELECT sift.col_promote('o_col');
SELECT sift.col_merge('o_col', random() < 0.3);
EOF
echo "CHECKPOINT;" >"$D/ckpt.sql"
FILES=(-f "$D/ins.sql@40" -f "$D/upd.sql@10" -f "$D/del.sql@5" -f "$D/vac.sql@1" -f "$D/maint.sql@2" -f "$D/ckpt.sql@1")

VERIFY="SELECT sift.col_verify('o_col', true), (SELECT count(*) FROM sift._test_col_tids('o_col', '')) = (SELECT count(*) FROM orders),
	(SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid')) = (SELECT count(*) FROM orders WHERE tenant_id < 10 AND status = 'paid'),
	pg_relation_size('o_col') / 1048576, typeof_same(), gone_same()"

for i in $(seq "$ITERS"); do
	crash_cycle crash "iteration $i" 3 8 "${FILES[@]}" || break
	out=$(sql crash "$VERIFY" 2>&1)
	IFS='|' read -r _ same same_filtered mb typed goneok <<<"$out"
	used=$(node_mb crash)
	elems=$(elem_groups crash on)
	if [[ $same == t && $same_filtered == t && $typed == t && $goneok == t && $used -lt $MAX_MB && -n $elems && $elems == "$(elem_groups crash off)" ]]; then
		ok "iteration $i: verified, index $mb MB, node $used MB"
	else
		not_ok "iteration $i"
		diag "$out"
		break
	fi
done
sql crash "VACUUM (index_cleanup on, disable_page_skipping) orders" "SELECT sift.col_merge('o_col', true)" "SELECT sift.col_merge('o_col', true)" >/dev/null
check "the element cubes of the merged segment answer the groups of the elements" test "$(elem_groups crash on)" == "$(elem_groups crash off)"
check "the element cubes answer units of the merged segment" grep -qE "Units From Pre-aggregates: [1-9]" <<<"$(elem_groups crash on explain)"
check "the soft-delete filter returns what the heap returns" test "$(sql crash "SELECT gone_same()")" == t
check "and the exceptions of the merged segment answer its units" grep -qE "Units Net Of Exceptions: [1-9]" <<<"$(sql crash "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT status, count(*), sum(qty) FROM orders WHERE gone IS NULL GROUP BY 1")"
check "a final merge verifies" test -z "$(sql crash "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "and leaves one immutable segment" test "$(sql crash "SELECT count(*) FROM sift.col_segments('o_col') WHERE kind = 'immutable'")" == 1
finish
