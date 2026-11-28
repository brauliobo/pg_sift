-- jsonb_each and jsonb_each_text of a map `col.* float8` of a sift_col index of format major 2: the cubes of the members of the map (SEC_PREAGG version 2) answer the granules that are clean, and a
-- granule that is not (a dead row, a pending delete, a value or a document the leaves cannot hold, a heap page that is not all-visible) is read alone. Every query is run with the node and
-- without it (sift.enable_agg) and the two answers are compared, errors included; the first word says they agree, the second that granules of the answer came from the cubes (preagg) or none did (scan).
-- The plan comes from the cost of the pages, not from a setting that forces it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;
SELECT current_setting('sift.test_create_format1', true) IS NOT NULL AS testing \gset
\if :testing
SET sift.test_create_format1 = off;
\endif

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(r::text, ''|'' ORDER BY r::text), '''')) FROM (' || q || ') r' INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'error ' || SQLSTATE || ' ' || SQLERRM;
END $$;

CREATE FUNCTION preagg_granules(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
		n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		n := n + coalesce(substring(l from 'preagg=(\d+)')::int, 0);
	END LOOP;
	RETURN n;
EXCEPTION WHEN OTHERS THEN
	RETURN 0;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; n int;
BEGIN
	SET LOCAL sift.enable_agg = on;
	a := answer(q);
	n := preagg_granules(q);
	SET LOCAL sift.enable_agg = off;
	b := answer(q);
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN n > 0 THEN 'preagg' ELSE 'scan' END) || ' ' || (CASE WHEN a LIKE 'error %' THEN a ELSE 'ok' END);
END $$;

-- Q-01. The members of 12000 documents of five to six keys, in granules of a part with cubes: the numbers are binary fractions, so that the sums do not depend on the order of the additions
CREATE TABLE mc (id int PRIMARY KEY, g int, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mc SELECT i, i % 7,
	CASE i % 10
		WHEN 0 THEN NULL
		WHEN 1 THEN '{}'
		WHEN 2 THEN jsonb_build_object('m_total', i * 0.5, 'packaging', i % 7)
		WHEN 3 THEN jsonb_build_object('check_a', i % 5, 'check_b', i % 3 - 1, 'tip', i % 4)
		WHEN 4 THEN jsonb_build_object('m_total', i * 0.25, 'check_a', i % 11)
		WHEN 5 THEN jsonb_build_object('packaging', i % 9, 'tip', 1.5)
		WHEN 6 THEN jsonb_build_object('m_total', i, 'packaging', i % 2, 'check_b', i % 6)
		WHEN 7 THEN jsonb_build_object('tip', i % 13)
		WHEN 8 THEN jsonb_build_object('m_total', -i, 'checkout', i % 4)
		ELSE jsonb_build_object('m_total', i * 1.5, 'packaging', 0, 'check_a', 7, 'check_b', 3, 'tip', 5)
	END
FROM generate_series(1, 12000) i;
CREATE INDEX mc_col ON mc USING sift_col (id, g, metrics) WITH (paths = 'metrics.* float8');
VACUUM mc;
ANALYZE mc;

CREATE TABLE mi (id int PRIMARY KEY, c jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mi SELECT i, CASE WHEN i % 8 = 0 THEN NULL ELSE jsonb_build_object('a', i % 5, 'b', i % 3 - 1, 'z', i) END FROM generate_series(1, 8000) i;
CREATE INDEX mi_col ON mi USING sift_col (id, c) WITH (paths = 'c.* int4');
VACUUM mi;
ANALYZE mi;

-- Q-02. The coverage row AP10 and the other ways to write it
SELECT m.key, count(*), sum(m.value::float8), avg(m.value::float8), min(m.value::float8), max(m.value::float8)
FROM mc o CROSS JOIN LATERAL jsonb_each_text(o.metrics) m WHERE m.key IN ('m_total', 'packaging') OR m.key LIKE 'check%' GROUP BY m.key ORDER BY 1;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT m.key, count(*), sum(m.value::float8), avg(m.value::float8), min(m.value::float8), max(m.value::float8) FROM mc o CROSS JOIN LATERAL jsonb_each_text(o.metrics) m WHERE m.key IN (''m_total'', ''packaging'') OR m.key LIKE ''check%'' GROUP BY m.key',
	'SELECT kv.key, avg(kv.value::float8), min(kv.value::float8), max(kv.value::float8), sum(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1',
	'SELECT kv.key, avg((kv.value #>> ''{}'')::float8), count(*) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1',
	'SELECT m.key, count(*), sum(m.value::float8) FROM mc o, jsonb_each_text(o.metrics) m GROUP BY 1',
	'SELECT count(*), sum(kv.value::float8), avg(kv.value::float8), min(kv.value::float8), max(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv',
	'SELECT count(*), count(kv.key), count(kv.value) FROM mc o, jsonb_each(o.metrics) kv',
	'SELECT kv.key, count(kv.value), count(kv.key) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1',
	'SELECT sum(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key LIKE ''check%'''
]) q;

-- Q-03. The tests on the key: lists, patterns of a literal (prefix, suffix), their OR, and what the cubes do not test (the value, a pattern with _ inside) scans
SELECT same(q) FROM unnest(ARRAY[
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key IN (''tip'', ''check_a'') GROUP BY 1',
	'SELECT kv.key, sum(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key LIKE ''check%'' GROUP BY 1',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key IN (''tip'') OR kv.key LIKE ''m_%'' GROUP BY 1',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key IN (''nokey'') GROUP BY 1',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key = ''tip'' GROUP BY 1',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.value::float8 > 3 GROUP BY 1',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key LIKE ''%a'' GROUP BY 1',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key LIKE ''c_eck%'' GROUP BY 1',
	'SELECT kv.key, sum(kv.value::float8) FILTER (WHERE kv.key LIKE ''m%'') FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1'
]) q;

-- Q-04. What the cubes of the members do not answer: a column of the table in the statement, another type than the leaf's, a LEFT JOIN, the value as it is
SELECT same(q) FROM unnest(ARRAY[
	'SELECT o.g, kv.key, sum(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1, 2',
	'SELECT kv.key, count(*) FROM mc o, jsonb_each(o.metrics) kv WHERE o.g < 3 GROUP BY 1',
	'SELECT kv.key, sum(kv.value::int4) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key = ''packaging'' GROUP BY 1',
	'SELECT o.g, count(*), count(kv.key) FROM mc o LEFT JOIN LATERAL jsonb_each(o.metrics) kv ON true GROUP BY 1',
	'SELECT kv.key, count(DISTINCT kv.value) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1'
]) q;

-- Q-05. A map of integers: the sums are exact
SELECT same(q) FROM unnest(ARRAY[
	'SELECT kv.key, sum(kv.value::int4), count(*), min(kv.value::int4), max(kv.value::int4) FROM mi o, jsonb_each(o.c) kv GROUP BY 1',
	'SELECT m.key, sum(m.value::int4), avg(m.value::int4) FROM mi o, jsonb_each_text(o.c) m GROUP BY 1',
	'SELECT kv.key, sum(kv.value::float8) FROM mi o, jsonb_each(o.c) kv GROUP BY 1'
]) q;

-- Q-05b. A float sum with sift.agg_exact_float adds in the order of the rows, which the sums of a cube do not: it is scanned, and the counts and the extremes are not
SET sift.agg_exact_float = on;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT kv.key, sum(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1',
	'SELECT kv.key, avg(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1',
	'SELECT kv.key, count(*), min(kv.value::float8), max(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1',
	'SELECT kv.key, sum(kv.value::int4) FROM mi o, jsonb_each(o.c) kv GROUP BY 1'
]) q;
RESET sift.agg_exact_float;

-- Q-06. Deletes: a granule with a dead row is read alone, the others are still answered from the cubes
CREATE FUNCTION run_all() RETURNS TABLE (r text) LANGUAGE sql AS $$
	SELECT same('SELECT kv.key, count(*), sum(kv.value::float8), min(kv.value::float8), max(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv GROUP BY 1')
	UNION ALL SELECT same('SELECT m.key, count(*), avg(m.value::float8) FROM mc o, jsonb_each_text(o.metrics) m WHERE m.key IN (''m_total'', ''packaging'') OR m.key LIKE ''check%'' GROUP BY 1') $$;
SELECT * FROM run_all();
DELETE FROM mc WHERE id IN (5, 6, 7, 9002);
SELECT * FROM run_all();
VACUUM mc;
SELECT * FROM run_all();

-- Q-07. Updates and an aborted transaction: the new version is a row of the heap past the cubes, and the old one is dead
UPDATE mc SET metrics = jsonb_build_object('m_total', 3, 'tip', 0.25) WHERE id IN (100, 200, 11000);
SELECT * FROM run_all();
VACUUM mc;
SELECT * FROM run_all();
BEGIN;
INSERT INTO mc SELECT i, 1, jsonb_build_object('m_total', 1, 'aborted', 2) FROM generate_series(50001, 50100) i;
ROLLBACK;
SELECT * FROM run_all();
VACUUM mc;
SELECT * FROM run_all();

-- Q-08. Rows in the slab, not yet in a part, and rows of the open transaction
INSERT INTO mc SELECT i, i % 7, jsonb_build_object('m_total', i * 0.5, 'slab', i % 3) FROM generate_series(60001, 60500) i;
SELECT * FROM run_all();
BEGIN;
INSERT INTO mc SELECT i, 2, jsonb_build_object('m_total', 4, 'open', 1) FROM generate_series(70001, 70050) i;
SELECT * FROM run_all();
ROLLBACK;
VACUUM mc;
SELECT * FROM run_all();

-- Q-08b. Rows past the index are parts of their own, with cubes of their own, and maintenance joins the parts and the cubes of their members
INSERT INTO mc SELECT i, i % 7, jsonb_build_object('m_total', i * 0.5, 'packaging', i % 5, 'late', i % 2) FROM generate_series(80001, 86000) i;
VACUUM mc;
SELECT * FROM run_all();
SELECT sift.maintain('mc_col') IS NOT NULL AS maintained;
VACUUM mc;
SELECT * FROM run_all();
SELECT same('SELECT kv.key, count(*), sum(kv.value::float8) FROM mc o, jsonb_each(o.metrics) kv WHERE kv.key IN (''late'', ''m_total'') GROUP BY 1') AS late_keys;

-- Q-09. A value or a document the leaves cannot hold, in a granule the index was built with: a string, a number past the range of a float8, a JSON null, the text of NaN, an array, a scalar.
-- The cube leaves the granule out and it is read; the answer, the error included, is the heap's. The other granules of the part are answered from their cubes.
CREATE FUNCTION mx_all(t regclass) RETURNS TABLE (r text) LANGUAGE sql AS $$
	SELECT same('SELECT kv.key, count(*), sum(kv.value::float8) FROM ' || t || ' o, jsonb_each(o.metrics) kv GROUP BY 1')
	UNION ALL SELECT same('SELECT m.key, count(*), sum(m.value::float8) FROM ' || t || ' o, jsonb_each_text(o.metrics) m GROUP BY 1')
	UNION ALL SELECT same('SELECT kv.key, count(*) FROM ' || t || ' o, jsonb_each(o.metrics) kv GROUP BY 1')
	UNION ALL SELECT same('SELECT count(*) FROM ' || t || ' o, jsonb_each(o.metrics) kv') $$;

CREATE TABLE mb (id int PRIMARY KEY, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mb SELECT i, CASE i
		WHEN 100 THEN jsonb_build_object('a', 'text', 'b', 1)
		WHEN 200 THEN jsonb_build_object('a', 1e400::numeric, 'b', 1)
		WHEN 300 THEN jsonb_build_object('a', NULL, 'b', 2)
		WHEN 400 THEN jsonb_build_object('a', 'NaN', 'b', 'Infinity')
		ELSE jsonb_build_object('a', i % 9, 'b', i % 4 + 0.5) END
FROM generate_series(1, 16000) i;
CREATE INDEX mb_col ON mb USING sift_col (id, metrics) WITH (paths = 'metrics.* float8');
VACUUM mb;
ANALYZE mb;
SELECT * FROM mx_all('mb');
DELETE FROM mb WHERE id IN (100, 200, 400);
VACUUM mb;
SELECT * FROM mx_all('mb');
DELETE FROM mb WHERE id = 300;
VACUUM mb;
SELECT * FROM mx_all('mb');

CREATE TABLE md (id int PRIMARY KEY, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO md SELECT i, CASE i WHEN 100 THEN '[1, 2]' WHEN 101 THEN '5' ELSE jsonb_build_object('a', i % 9, 'b', i % 4 + 0.5) END FROM generate_series(1, 16000) i;
CREATE INDEX md_col ON md USING sift_col (id, metrics) WITH (paths = 'metrics.* float8');
VACUUM md;
ANALYZE md;
SELECT * FROM mx_all('md');
DELETE FROM md WHERE id IN (100, 101);
VACUUM md;
SELECT * FROM mx_all('md');

-- Q-10. The counters: EXPLAIN (ANALYZE) names the granules the cubes answered
SELECT preagg_granules('SELECT kv.key, count(*), sum(kv.value::float8) FROM md o, jsonb_each(o.metrics) kv GROUP BY 1') > 0 AS cubes_answer;

DROP FUNCTION same(text), answer(text), preagg_granules(text), run_all(), mx_all(regclass);
DROP TABLE mc, mi, mb, md;
DROP EXTENSION pg_sift;
