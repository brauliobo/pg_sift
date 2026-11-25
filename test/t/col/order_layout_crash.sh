#!/usr/bin/env bash
# kill -9 of the postmaster and all backends in the middle of concurrent inserts, updates that change the key, deletes, VACUUM, promotion and merge of a sift_col index
# whose order arrays are compact (order_layout = 'compact', the chunks of the order keep the bucket of a key and read the exact one from the heap row), then crash recovery
# and checks: every segment verifies, the segments are written compact, and the ordered reads (ASC and DESC, with an OFFSET and a range) return what the heap returns,
# which the dead rows a crash orphans, the reused addresses and the pruned chains of the heap must not change. SIFT_COL_CRASH_ITERS (default 20, 3 quick).
. "$(dirname "$0")/lib.bash"
. "$(dirname "$0")/lib_col.bash"

ITERS=$(knob SIFT_COL_CRASH_ITERS 20 3)
plan $((ITERS + 4))
node_init crash 1 "checkpoint_timeout = 30s
max_wal_size = 256MB
autovacuum = off
sift.test_col_gorder_ends = 64"
node_start crash || bail "start failed"
sql crash "CREATE EXTENSION pg_sift_test CASCADE" \
	"SELECT setseed(0.42)" \
	"CREATE TABLE orders (id bigserial PRIMARY KEY, ts timestamp, qty bigint)" \
	"INSERT INTO orders (ts, qty) SELECT TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(1, 30000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (id, ts, qty) WITH (write_buffer_limit = 64, order_array_columns = 'ts', order_layout = 'compact')" \
	"CREATE FUNCTION ordered_same() RETURNS boolean LANGUAGE plpgsql AS \$\$
	DECLARE q text; a text; b text; qs text[] = ARRAY[
		'SELECT ts AS k FROM orders ORDER BY ts LIMIT 40 OFFSET 900',
		'SELECT ts AS k FROM orders ORDER BY ts DESC LIMIT 40 OFFSET 5000',
		'SELECT ts AS k FROM orders WHERE ts >= TIMESTAMP ''2026-04-01'' ORDER BY ts LIMIT 40 OFFSET 3000',
		'SELECT ts AS k FROM orders WHERE ts <= TIMESTAMP ''2026-08-01'' ORDER BY ts DESC LIMIT 100'];
	BEGIN
		FOREACH q IN ARRAY qs LOOP
			PERFORM set_config('sift.enable_agg', 'on', true);
			EXECUTE 'SELECT string_agg(k::text, '','') FROM (' || q || ') s' INTO a;
			PERFORM set_config('sift.enable_agg', 'off', true);
			EXECUTE 'SELECT string_agg(k::text, '','') FROM (' || q || ') s' INTO b;
			IF a IS DISTINCT FROM b THEN RETURN false; END IF;
		END LOOP;
		RETURN true;
	END \$\$" >/dev/null || bail "setup failed"
# 41 is the minor of the compact chunks; a segment with a seekable chunk (47, col_gorder_seek) is stamped the newer one
check "the segments of the index are compact" test "$(sql crash "SELECT count(*) FROM sift.col_segments('o_col') WHERE kind = 'immutable' AND format >= 41")" -gt 0

D=$(dir crash)
cat >"$D/ins.sql" <<'SQL'
INSERT INTO orders (ts, qty) SELECT TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(1, 20) i;
SQL
cat >"$D/upd.sql" <<'SQL'
\set id random(1, 100000)
UPDATE orders SET ts = ts + INTERVAL '2 days' WHERE id BETWEEN :id AND :id + 200;
UPDATE orders SET qty = qty + 1 WHERE id BETWEEN :id AND :id + 100;
SQL
cat >"$D/del.sql" <<'SQL'
\set id random(1, 100000)
DELETE FROM orders WHERE id BETWEEN :id AND :id + 30;
SQL
echo "VACUUM orders;" >"$D/vac.sql"
cat >"$D/maint.sql" <<'SQL'
SELECT sift.col_promote('o_col');
SELECT sift.col_merge('o_col', random() < 0.3);
SQL
echo "CHECKPOINT;" >"$D/ckpt.sql"
FILES=(-f "$D/ins.sql@40" -f "$D/upd.sql@10" -f "$D/del.sql@5" -f "$D/vac.sql@1" -f "$D/maint.sql@2" -f "$D/ckpt.sql@1")

for i in $(seq "$ITERS"); do
	crash_cycle crash "iteration $i" 3 8 "${FILES[@]}" || break
	out=$(sql crash "SELECT sift.col_verify('o_col', true), ordered_same()" 2>&1)
	IFS='|' read -r _ same <<<"$out"
	if [[ $same == t ]]; then
		ok "iteration $i: verified, the ordered reads return the heap's"
	else
		not_ok "iteration $i"
		diag "$out"
		break
	fi
done
sql crash "VACUUM (index_cleanup on, disable_page_skipping) orders" "SELECT sift.col_promote('o_col')" "SELECT sift.col_merge('o_col', true)" >/dev/null
check "a final merge verifies" test -z "$(sql crash "SELECT sift.col_verify('o_col', true)" 2>&1)"
check "and its ordered reads return the heap's" test "$(sql crash "SELECT ordered_same()")" == t
check "the merged segment is compact" test "$(sql crash "SELECT count(*) FROM sift.col_segments('o_col') WHERE kind = 'immutable' AND format >= 41")" -gt 0
finish
