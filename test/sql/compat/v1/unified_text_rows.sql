-- Format 1 on the unified aggregate scan (sift.test_unified_agg): the best rows of an ORDER BY .. LIMIT of a text search come from the rows the matches of the search name, chunk by chunk, and are
-- those the heap gives. Every statement is run with the node and without it (sift.enable_agg) and the answers are compared.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = on;
SET sift.test_unified_agg = on;
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE t (id int PRIMARY KEY, ts date, amount float8, gone timestamptz, memo text);
INSERT INTO t
SELECT i, date '2024-01-01' + (i * 7) % 300, i % 17 + 0.25 * (i % 4), CASE WHEN i % 37 = 0 THEN now() END,
	(ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + i % 8] || ' ' || (ARRAY['cable', 'wire', 'plug', 'adapter', 'switch', 'panel', 'socket', 'fuse'])[1 + (i * 3 + 1) % 8]
FROM generate_series(1, 4000) i;
CREATE INDEX t_col ON t USING sift_col (id, ts, amount, gone);
CREATE INDEX t_memo ON t USING sift (memo);
VACUUM (FREEZE, ANALYZE) t;

-- the node the plan of q has, and whether its rows are those of the heap (sift.enable_agg = off), in the order of the statement
CREATE FUNCTION check_q(q text) RETURNS TABLE (node text, same bool) LANGUAGE plpgsql AS $$
DECLARE l text; p text := ''; x text; y text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP p := p || l || E'\n'; END LOOP;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n''), '''') FROM (%s) r', q) INTO x;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n''), '''') FROM (%s) r', q) INTO y;
	SET LOCAL sift.enable_agg = on;
	node := coalesce((regexp_match(p, 'Custom Scan \((Sift\w+)\)'))[1], 'none');
	same := x = y;
	RETURN NEXT;
END $$;

SELECT * FROM check_q($$SELECT id, ts, amount FROM t WHERE memo @@> 'cable wire' AND gone IS NULL ORDER BY ts DESC, id DESC LIMIT 8$$);
SELECT * FROM check_q($$SELECT id, ts FROM t WHERE memo @@> 'plug' ORDER BY ts, id LIMIT 20 OFFSET 5$$);
SELECT * FROM check_q($$SELECT id, ts FROM t WHERE memo @@> 'panel' AND ts >= date '2024-06-01' ORDER BY ts DESC, id LIMIT 3$$);
SELECT * FROM check_q($$SELECT id, ts FROM t WHERE memo @@> 'cable wire fuse' ORDER BY ts, id LIMIT 5$$);

DROP TABLE t;
DROP FUNCTION check_q;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
