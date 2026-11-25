-- SELECT DISTINCT dimensions under the aggregate scan, on a Format 2.0 table and on a Format 1 table with sift.test_unified_agg = on: a CROSS JOIN of DISTINCT sub-selects with a LEFT, RIGHT, FULL,
-- inner or semi join of the fact table, a DISTINCT over a member of a document, and the DISTINCT alone. The planner offers the DISTINCT stage no grouping data, so none of these may read the HAVING of
-- the statement. Every statement runs on the node (when the plan has one) and with sift.enable_agg = off, and the rows must be the same.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE ev_f2 (id int PRIMARY KEY, kind text, skew int, qty int, doc jsonb) WITH (autovacuum_enabled = false);
CREATE TABLE ev_f1 (LIKE ev_f2 INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO ev_f2
SELECT i, CASE WHEN i % 19 = 0 THEN NULL ELSE 'cat_' || chr(97 + i % 4) END, CASE WHEN i % 23 = 0 THEN NULL ELSE 1 + i % 8 END, i % 11,
       CASE WHEN i % 13 = 0 THEN NULL ELSE jsonb_build_object('k', 'g' || i % 5, 'm', i % 7) END
FROM generate_series(1, 3000) i;
INSERT INTO ev_f1 SELECT * FROM ev_f2;
SET sift.test_create_format1 = off;
CREATE INDEX ev_f2_col ON ev_f2 USING sift_col (id, kind, skew, qty, doc) WITH (paths = 'doc->k text, doc->m float8');
SET sift.test_create_format1 = on;
CREATE INDEX ev_f1_col ON ev_f1 USING sift_col (id, kind, skew, qty, doc) WITH (paths = 'doc->k text, doc->m float8');
VACUUM (FREEZE, ANALYZE) ev_f2;
VACUUM (FREEZE, ANALYZE) ev_f1;
CREATE TABLE spine (n int);
INSERT INTO spine SELECT 2 * i FROM generate_series(1, 8) i;
ANALYZE spine;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6)%' THEN engine := 'SiftAggV6'; END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_heap := (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE stmts (id int, q text);
INSERT INTO stmts VALUES
(1, 'SELECT k.kind, s.skew, count(e.id) AS n, sum(e.qty) AS q FROM (SELECT DISTINCT kind FROM @) k CROSS JOIN (SELECT DISTINCT skew FROM @ WHERE skew <= 5) s LEFT JOIN @ e ON e.kind = k.kind AND e.skew = s.skew GROUP BY 1, 2'),
(2, 'SELECT k.kind, s.skew, count(e.id) AS n FROM (SELECT DISTINCT kind FROM @) k CROSS JOIN (SELECT DISTINCT skew FROM @ WHERE skew <= 5) s RIGHT JOIN @ e ON e.kind = k.kind AND e.skew = s.skew GROUP BY 1, 2'),
(3, 'SELECT k.kind, s.skew, count(e.id) AS n FROM (SELECT DISTINCT kind FROM @) k CROSS JOIN (SELECT DISTINCT skew FROM @ WHERE skew <= 5) s FULL JOIN @ e ON e.kind = k.kind AND e.skew = s.skew GROUP BY 1, 2'),
(4, 'SELECT k.kind, s.skew, count(e.id) AS n FROM (SELECT DISTINCT kind FROM @) k CROSS JOIN (SELECT DISTINCT skew FROM @ WHERE skew <= 5) s JOIN @ e ON e.kind = k.kind AND e.skew = s.skew GROUP BY 1, 2'),
(5, 'SELECT k.kind, count(*) AS n FROM (SELECT DISTINCT kind FROM @) k CROSS JOIN (SELECT DISTINCT skew FROM @) s GROUP BY 1'),
(6, 'SELECT e.kind, count(*) AS n FROM @ e WHERE e.skew IN (SELECT DISTINCT skew FROM @ WHERE qty > 8) GROUP BY 1'),
(7, 'SELECT e.kind, count(*) AS n FROM @ e WHERE NOT EXISTS (SELECT 1 FROM (SELECT DISTINCT skew FROM @ WHERE qty > 9) s WHERE s.skew = e.skew) GROUP BY 1'),
(8, 'SELECT DISTINCT kind FROM @'),
(9, 'SELECT DISTINCT kind, skew FROM @ WHERE qty > 3'),
(10, 'SELECT DISTINCT doc->>''k'' FROM @'),
(11, 'SELECT DISTINCT doc->>''k'', kind FROM @ WHERE qty > 5'),
(12, 'SELECT k.g, e.kind, count(*) AS n FROM (SELECT DISTINCT doc->>''k'' AS g FROM @) k LEFT JOIN @ e ON e.doc->>''k'' = k.g GROUP BY 1, 2'),
(13, 'SELECT s.n, count(e.id) AS n FROM spine s LEFT JOIN @ e ON e.skew * 2 = s.n GROUP BY 1'),
(14, 'SELECT s.n, count(e.id) AS n, sum(e.qty) AS q FROM spine s JOIN @ e ON e.skew + e.qty = s.n GROUP BY 1'),
(15, 'SELECT s.n, count(e.id) AS n FROM spine s LEFT JOIN @ e ON e.skew * 2 = s.n AND e.qty > 5 GROUP BY 1');

SELECT 'f2' AS fmt, id, (same(replace(q, '@', 'ev_f2'))).* FROM stmts ORDER BY id;
SELECT 'f1' AS fmt, id, (same(replace(q, '@', 'ev_f1'))).* FROM stmts ORDER BY id;

DROP FUNCTION same(text);
DROP TABLE stmts, spine, ev_f2, ev_f1;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
