-- The passes that rewrite a query before it is planned (lateral sub-selects pulled up, single-reference CTEs inlined, the shared CTE walk, the json_build_object swap) look for
-- sub-selects, CTE references and calls. A list of constants (`k IN (1, 2, ..)`) holds none, so they do not walk it: the nodes they visit do not grow with the list.
-- sift._test_prepass_nodes (pg_sift_test) counts the nodes visited.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE pc_fact (id int, k int, v int);
INSERT INTO pc_fact SELECT i, i % 5000, i % 7 FROM generate_series(1, 20000) i;
ANALYZE pc_fact;
CREATE INDEX pc_fact_col ON pc_fact USING sift_col (id, k, v) WITH (rollup_dims = 'off');

-- a statement with a single-reference CTE, a sub-select, a lateral and a json_build_object call: every pass has something to look at
-- a statement with a single-reference CTE, a sub-select, a lateral and a json_build_object call: every pass has something to look at; tail is added to the list
CREATE FUNCTION pc_stmt(n int, tail text) RETURNS text LANGUAGE sql AS $f$
SELECT format($q$WITH c AS (SELECT * FROM pc_fact WHERE k IN (%s%s))
SELECT json_build_object('n', count(*), 'm', (SELECT max(v) FROM c)), x.s
FROM c, LATERAL (SELECT sum(id) s FROM pc_fact f WHERE f.k = c.k) x GROUP BY x.s ORDER BY x.s LIMIT 5$q$,
  (SELECT string_agg(i::text, ',') FROM generate_series(1, n) i), tail)
$f$;

CREATE FUNCTION pc_nodes(n int, tail text DEFAULT '') RETURNS bigint LANGUAGE plpgsql AS $f$
BEGIN
	PERFORM sift._test_prepass_nodes(true);
	EXECUTE 'EXPLAIN (COSTS OFF) ' || pc_stmt(n, tail);
	RETURN sift._test_prepass_nodes(true);
END $f$;

SELECT pc_nodes(10) AS discard, pc_nodes(5000) AS discard2 \gset
SELECT pc_nodes(10) > 0 AS passes_ran;
SELECT pc_nodes(5000) - pc_nodes(10) < 50 AS nodes_do_not_grow_with_the_list;

-- a list that holds one element that is not a constant is walked (it may hold a sub-select), and the sub-select is found as before
SELECT pc_nodes(5000, ', (SELECT 3)') > 5000 AS a_list_with_a_sublink_is_walked;
SELECT count(*) AS with_sublink FROM pc_fact WHERE k IN (1, 2, (SELECT 3));
SELECT count(*) AS with_consts FROM pc_fact WHERE k IN (1, 2, 3);

DROP FUNCTION pc_nodes(int, text), pc_stmt(int, text);
DROP TABLE pc_fact;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
