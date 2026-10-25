#!/usr/bin/env bash
# kill -9 of the postmaster and all backends in the middle of concurrent inserts, updates, deletes, VACUUM, promotion and merge of a sift_col index
# with order_columns (the heads of the sorted runs, column format minor 18, and with order_array_columns the global order of the keys, minor 21), then crash recovery and checks: every segment and every head verify,
# ORDER BY .. LIMIT n over the index returns what the normal plan returns (the units answered from a head are those that are clean), and the space
# stays bounded. SIFT_COL_CRASH_ITERS (default 20, 3 quick).
. "$(dirname "$0")/lib.bash"

ITERS=$(knob SIFT_COL_CRASH_ITERS 20 3)
MAX_MB=$(knob SIFT_CRASH_MAX_MB 4096 1024)
plan $((ITERS + 5))
node_init crash 1 "checkpoint_timeout = 30s
max_wal_size = 256MB
autovacuum = off"
node_start crash || bail "start failed"
sql crash "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE FUNCTION topn_same() RETURNS bool LANGUAGE plpgsql AS \$\$
		DECLARE col text; dir text; lim int; g text; q text; a text; b text;
		BEGIN
			SET LOCAL max_parallel_workers_per_gather = 0;
			FOREACH col IN ARRAY ARRAY['k', 'v'] LOOP
			FOREACH dir IN ARRAY ARRAY['ASC', 'DESC'] LOOP
			FOREACH lim IN ARRAY ARRAY[1, 32, 100] LOOP
			FOREACH g IN ARRAY ARRAY['on', 'off'] LOOP
				q := format('SELECT coalesce(string_agg(%1\$s::text, '','' ORDER BY pos), '''') FROM (SELECT row_number() OVER () AS pos, %1\$s FROM (SELECT %1\$s FROM t ORDER BY %1\$s %2\$s LIMIT %3\$s) x) y', col, dir, lim);
				PERFORM set_config('sift.test_topn_gorder', g, true);
				SET LOCAL sift.enable_agg = on;
				EXECUTE q INTO a;
				SET LOCAL sift.enable_agg = off;
				EXECUTE q INTO b;
				IF a IS DISTINCT FROM b THEN RETURN false; END IF;
			END LOOP; END LOOP; END LOOP; END LOOP;
			RETURN true;
		END \$\$" \
	"CREATE TABLE t (id bigserial PRIMARY KEY, k int NOT NULL, v int NOT NULL, pad text)" \
	"ALTER TABLE t ALTER COLUMN pad SET STORAGE PLAIN" \
	"INSERT INTO t (k, v, pad) SELECT (hashint4(i) & 2147483647) % 100000, i, repeat('x', 400) FROM generate_series(1, 60000) i" \
	"CREATE INDEX t_col ON t USING sift_col (k, v) WITH (write_buffer_limit = 64, order_columns = 'k, v', order_array_columns = 'k, v')" \
	"VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) t" >/dev/null || bail "setup failed"
check "the segments carry the heads" test "$(sql crash "SELECT string_agg(DISTINCT format::text, ',') FROM sift._test_col_formats('t_col')")" == 21
check "the top-N scan answers units from the heads" test "$(sql crash "SET max_parallel_workers_per_gather = 0" "SET sift.test_topn_gorder = off" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT k FROM t ORDER BY k LIMIT 10" | grep -c 'Units Answered By Run Head')" == 1
check "and reads the global order of the key" test "$(sql crash "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT k FROM t ORDER BY k LIMIT 100" | grep -c 'Global Order')" == 1

D=$(dir crash)
cat >"$D/ins.sql" <<'EOS'
INSERT INTO t (k, v, pad) SELECT (random() * 100000)::int, i, repeat('x', 400) FROM generate_series(1, 20) i;
EOS
cat >"$D/upd.sql" <<'EOS'
\set id random(1, 100000)
UPDATE t SET k = k + 1 WHERE id BETWEEN :id AND :id + 100;
EOS
cat >"$D/del.sql" <<'EOS'
\set id random(1, 100000)
DELETE FROM t WHERE id BETWEEN :id AND :id + 30;
EOS
echo "VACUUM t;" >"$D/vac.sql"
cat >"$D/maint.sql" <<'EOS'
SELECT sift.col_promote('t_col');
SELECT sift.col_merge('t_col', random() < 0.3);
EOS
echo "CHECKPOINT;" >"$D/ckpt.sql"
FILES=(-f "$D/ins.sql@40" -f "$D/upd.sql@10" -f "$D/del.sql@5" -f "$D/vac.sql@1" -f "$D/maint.sql@2" -f "$D/ckpt.sql@1")

VERIFY="SELECT sift.col_verify('t_col', true), (SELECT count(*) FROM sift._test_col_tids('t_col', '')) = (SELECT count(*) FROM t), pg_relation_size('t_col') / 1048576, topn_same()"
for i in $(seq "$ITERS"); do
	crash_cycle crash "iteration $i" 3 8 "${FILES[@]}" || break
	out=$(sql crash "$VERIFY" 2>&1)
	IFS='|' read -r _ same mb topn <<<"$out"
	used=$(node_mb crash)
	if [[ $same == t && $topn == t && $used -lt $MAX_MB ]]; then
		ok "iteration $i: verified, index $mb MB, node $used MB"
	else
		not_ok "iteration $i"
		diag "$out"
		break
	fi
done
sql crash "VACUUM (index_cleanup on, disable_page_skipping) t" "SELECT sift.col_merge('t_col', true)" "SELECT sift.col_merge('t_col', true)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) t" >/dev/null
check "a final merge verifies" test -z "$(sql crash "SELECT sift.col_verify('t_col', true)" 2>&1)"
check "and leaves one immutable segment with the heads, whose top-N answers are the heap's, with and without the global order" \
	test "$(sql crash "SELECT (SELECT count(*) FROM sift.col_segments('t_col') WHERE kind = 'immutable' AND format = 21) = 1 AND topn_same()")" == t
finish
