-- SubPlans and InitPlans in filters and aggregate FILTERs, joins on an expression of the fact side and a correlated lateral LIMIT 1 on Format 2.0: each statement runs with the cost settings
-- of the coverage proofs (a page costs 10000, no Memoize) and with sift.enable_agg = off, and the rows must be the same. The engine column is the plan of the first run (a lateral LIMIT 1 stays in
-- PostgreSQL's plan: a shape the node does not serve yet keeps its rows as the heap's).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
-- the library of the testing build creates the indexes of format major 1 by default; these are of format major 2, which the release library creates
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE TABLE o (id int PRIMARY KEY, tenant int, status text, placed date, total int, note text, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO o
SELECT i, 1 + i % 40, CASE WHEN i % 13 = 0 THEN NULL ELSE 's' || (i % 5) END, date '2024-01-01' + i % 700, i % 97, 'n' || i,
       jsonb_build_array(jsonb_build_object('category', 'c' || (i % 5), 'qty', i % 7), jsonb_build_object('category', 'c' || (i % 3), 'qty', i % 4))
FROM generate_series(1, 4000) i;
CREATE INDEX o_col ON o USING sift_col (id, tenant, status, placed, total, items) WITH (paths = 'items[]{category text, qty int4}');
VACUUM (FREEZE, ANALYZE) o;

CREATE TABLE t (id int PRIMARY KEY, kind text, country text);
INSERT INTO t SELECT i, CASE WHEN i % 4 = 0 THEN 'outlet' ELSE 'shop' || i % 3 END, CASE WHEN i % 5 = 0 THEN NULL ELSE 'c' || i % 3 END FROM generate_series(1, 40) i;
CREATE TABLE w (id int PRIMARY KEY, a int, b int, c text);
INSERT INTO w SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 9 END, i % 6, 'x' || i % 4 FROM generate_series(1, 40) i;
CREATE TABLE cm (category text, label text);
INSERT INTO cm VALUES ('c0', 'zero'), ('c1', 'one'), ('c2', 'two'), ('c3', 'three');
ANALYZE t; ANALYZE w; ANALYZE cm;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	SET LOCAL seq_page_cost = 10000;
	SET LOCAL random_page_cost = 10000;
	SET LOCAL enable_memoize = off;
	SET LOCAL enable_tidscan = off;
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
-- a hashed SubPlan: NOT IN and ANY of a sub-select, over one column and over a pair
(1, 'SELECT count(*) FROM o WHERE (tenant, total) NOT IN (SELECT a, b FROM w WHERE a IS NOT NULL)'),
(2, 'SELECT count(*) FROM o WHERE (tenant, total) NOT IN (SELECT id, 7 FROM t WHERE id IS NOT NULL)'),
(3, 'SELECT status, count(*) FROM o WHERE tenant IN (SELECT id FROM t WHERE kind = ''outlet'') OR total = 3 GROUP BY status'),
(4, 'SELECT count(*) FROM o WHERE tenant = ANY (ARRAY(SELECT a FROM w WHERE c = ''x1''))'),
(5, 'SELECT count(*) FROM o WHERE tenant NOT IN (SELECT a FROM w)'),
-- an InitPlan: a scalar sub-select compared with a column and used as an argument
(6, 'SELECT status, count(*) FROM o WHERE total > (SELECT avg(b) FROM w) GROUP BY status'),
(7, 'SELECT sum(total + (SELECT max(b) FROM w)) FROM o WHERE tenant < 20'),
-- a FILTER of an aggregate that reads sub-selects
(8, 'SELECT count(*) FILTER (WHERE tenant = ANY (ARRAY(SELECT a FROM w WHERE c = ''x1''))), count(*) FILTER (WHERE tenant <> ALL (ARRAY(SELECT a FROM w))), count(*) FILTER (WHERE NOT (tenant = ANY (ARRAY(SELECT a FROM w WHERE c = ''x2'')))), count(*) FILTER (WHERE total < 10 OR tenant = ANY (ARRAY(SELECT b FROM w))) FROM o'),
(9, 'SELECT status, count(*) FILTER (WHERE tenant IN (SELECT id FROM t WHERE country IS NULL)), sum(total) FILTER (WHERE tenant IN (SELECT id FROM t WHERE kind = ''outlet'')) FROM o GROUP BY status'),
-- a key of the join that is an expression of the columns of the fact side
(10, 'SELECT w.c, count(*) FROM o JOIN w ON w.c = lower(''X'' || (o.tenant % 4)) GROUP BY w.c'),
(11, 'SELECT t.kind, count(*) FROM o JOIN t ON t.id = o.tenant + o.total % 3 - 1 GROUP BY t.kind'),
(12, 'SELECT t.kind, count(*) FROM o LEFT JOIN t ON t.id = o.tenant * 2 - 40 GROUP BY t.kind'),
(13, 'SELECT w.c, t.kind, count(*) FROM o JOIN w ON w.c = lower(''X'' || (o.tenant % 4)) JOIN t ON t.id = o.tenant GROUP BY w.c, t.kind'),
(14, 'SELECT cm.label, count(*) FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) JOIN cm ON cm.category = lower(i.category) GROUP BY cm.label'),
(15, 'SELECT w.c, count(*) FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int) JOIN w ON w.id = o.tenant + i.qty - 3 GROUP BY w.c'),
-- a correlated lateral LIMIT 1 of a unique key and of a key with ties, inner and left, with keys that match no row and NULL keys
(16, 'SELECT w2.c, count(*) FROM o LEFT JOIN LATERAL (SELECT c FROM w WHERE w.id = o.tenant LIMIT 1) w2 ON true GROUP BY w2.c'),
(17, 'SELECT w2.c, count(*) FROM o JOIN LATERAL (SELECT c FROM w WHERE w.id = o.tenant LIMIT 1) w2 ON true GROUP BY w2.c'),
(18, 'SELECT w2.c, count(*) FROM o LEFT JOIN LATERAL (SELECT c FROM w WHERE w.id = o.tenant + 30 LIMIT 1) w2 ON true GROUP BY w2.c'),
(19, 'SELECT w2.b, count(*) FROM o LEFT JOIN LATERAL (SELECT b FROM w WHERE w.a = o.tenant % 9 ORDER BY b LIMIT 1) w2 ON true GROUP BY w2.b'),
(20, 'SELECT w2.kind, count(*) FROM o LEFT JOIN LATERAL (SELECT kind FROM t WHERE t.id = o.tenant LIMIT 1) w2 ON true GROUP BY w2.kind'),
-- an aggregate in the HAVING and in the ORDER BY of a join: a member of an equivalence class that is no key
(21, 'SELECT t.country, sum(o.total) FROM o JOIN t ON t.id = o.tenant GROUP BY 1 HAVING sum(o.total) > 100 ORDER BY 2 DESC LIMIT 3');

SELECT s.id, r.* FROM stmts s, LATERAL same(s.q) r ORDER BY s.id;

DROP FUNCTION same(text);
DROP TABLE stmts, o, t, w, cm;
DROP EXTENSION pg_sift;
