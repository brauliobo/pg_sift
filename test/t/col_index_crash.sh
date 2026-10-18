#!/usr/bin/env bash
# kill -9 of the postmaster and all backends in the middle of concurrent inserts, updates, deletes, VACUUM, promotion and merge of a
# sift_col index, then crash recovery and checks: the store and every segment verify, the index holds exactly the visible heap tuples
# with their values (heapallindexed), the column source returns what the heap returns, and the space stays bounded (the sweep reclaims
# what the crash orphaned). SIFT_COL_CRASH_ITERS (default 40, 4 quick).
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_COL_CRASH_ITERS 40 4)
MAX_MB=$(knob SIFT_CRASH_MAX_MB 4096 1024)
plan $((ITERS + 3))
node_init crash 1 "checkpoint_timeout = 30s
max_wal_size = 256MB
autovacuum = off"
node_start crash || bail "start failed"
sql crash "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, store_id int, status text, qty bigint, price float8, doc jsonb)" \
	"INSERT INTO orders (store_id, status, qty, price, doc) SELECT i % 50, (ARRAY['new','paid','void'])[1 + i % 3], i, i / 4.0,
		jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) FROM generate_series(1, 20000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (store_id, status, qty, price, doc)
		WITH (mutable_limit = 64, paths = 'doc.items[]{category text, qty int4}')" >/dev/null || bail "setup failed"
ok "setup"

D=$(dir crash)
cat >"$D/ins.sql" <<'EOF'
INSERT INTO orders (store_id, status, qty, price, doc) SELECT (random() * 50)::int, 'paid', i, i / 2.0,
	jsonb_build_object('items', jsonb_build_array(jsonb_build_object('category', 'c' || i % 5, 'qty', i % 7))) FROM generate_series(1, 20) i;
EOF
cat >"$D/upd.sql" <<'EOF'
\set id random(1, 100000)
UPDATE orders SET store_id = coalesce(store_id, 0) + 1 WHERE id BETWEEN :id AND :id + 200;
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
	(SELECT count(*) FROM sift._test_col_tids('o_col', '0:lt:10;1:eq:paid')) = (SELECT count(*) FROM orders WHERE store_id < 10 AND status = 'paid'),
	pg_relation_size('o_col') / 1048576"

for i in $(seq "$ITERS"); do
	crash_cycle crash "iteration $i" 3 8 "${FILES[@]}" || break
	out=$(sql crash "$VERIFY" 2>&1)
	IFS='|' read -r _ same same_filtered mb <<<"$out"
	used=$(node_mb crash)
	if [[ $same == t && $same_filtered == t && $used -lt $MAX_MB ]]; then
		ok "iteration $i: verified, index $mb MB, node $used MB"
	else
		not_ok "iteration $i"
		diag "$out"
		break
	fi
done
sql crash "SELECT sift.col_merge('o_col', true)" "SELECT sift.col_merge('o_col', true)" >/dev/null
check "a final merge verifies" test -z "$(sql crash "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "and leaves one immutable segment" test "$(sql crash "SELECT count(*) FROM sift.col_segments('o_col') WHERE kind = 'immutable'")" == 1
finish
