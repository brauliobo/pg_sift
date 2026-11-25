-- The text quals of a window over a sift_table (a text =, <>, LIKE, IN or an OR of them in the WHERE of a window Top-N) are quals of the SiftAggV6Window scan: the machine of agg/vexpr/vexpr.h runs the
-- clauses over numbers, and the clauses over text narrow the batches the source gives (agg/v6/source_quals.h). Every check runs the statement on the table and on a heap twin holding the
-- same rows, compares the rows as text, and asserts the node that read the table.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
\pset format unaligned
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET cpu_tuple_cost = 100;
SET random_page_cost = 10000;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;

CREATE TABLE wt (id int PRIMARY KEY, g int, k int, s text, v varchar(12)) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE wh (LIKE wt INCLUDING ALL);

CREATE VIEW wbulk AS SELECT i AS id, i % 7 AS g, (i * 37) % 101 AS k,
	CASE i % 9 WHEN 0 THEN NULL WHEN 1 THEN '' WHEN 2 THEN 'google.com/' || (i % 5) WHEN 3 THEN 'Google' WHEN 4 THEN 'a%b' WHEN 5 THEN 'é' || (i % 3) ELSE 'p' || (i % 11) END AS s,
	CASE WHEN i % 10 = 0 THEN NULL WHEN i % 10 = 1 THEN '' ELSE 'v' || (i % 4) END AS v
FROM generate_series(1, 5000) i;

INSERT INTO wt SELECT * FROM wbulk;
INSERT INTO wh SELECT * FROM wbulk;
SELECT sift.table_maintain('wt') IS NOT NULL AS maintained;
ANALYZE wt;
ANALYZE wh;

CREATE FUNCTION plan_of(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; plan text := '';
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (COSTS off) ' || sql LOOP plan := plan || r || E'\n'; END LOOP;
	RETURN plan;
END $$;

CREATE FUNCTION text_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE t text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (%s) x', q) INTO t;
	RETURN t;
END $$;

CREATE FUNCTION same(name text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text := plan_of(replace(q, '@T@', 'wt'));
BEGIN
	IF text_of(replace(q, '@T@', 'wt')) IS DISTINCT FROM text_of(replace(q, '@T@', 'wh')) THEN
		RAISE EXCEPTION 'FAILED %: the rows of the table are not those of the heap twin', name;
	END IF;
	IF plan NOT LIKE '%Custom Scan (SiftAggV6Window)%' OR plan LIKE '%Seq Scan%' THEN RAISE EXCEPTION 'FAILED %: SiftAggV6Window did not read the table: %', name, plan; END IF;
	RETURN name || ': ok';
END $$;

\echo == the Top-N of a partition with a text qual
SELECT same('text =', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s = ''p3'') w WHERE rn <= 3');
SELECT same('text <>', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s <> '''') w WHERE rn <= 3');
SELECT same('text like', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s LIKE ''%google%'') w WHERE rn <= 4');
SELECT same('text not like', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s NOT LIKE ''%google%'') w WHERE rn <= 2');
SELECT same('text in', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s IN (''p3'', ''Google'', ''a%b'') ) w WHERE rn <= 3');
SELECT same('text range', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s >= ''g'' AND s < ''q'') w WHERE rn <= 3');
SELECT same('varchar =', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE v = ''v2'') w WHERE rn <= 3');
SELECT same('two text quals', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s <> '''' AND v <> '''') w WHERE rn <= 3');
SELECT same('text or text', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s = ''p3'' OR v = ''v1'') w WHERE rn <= 3');
SELECT same('text and number', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s <> '''' AND k < 50) w WHERE rn <= 3');
SELECT same('no match', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s = ''nosuch'') w WHERE rn <= 3');

\echo == other window functions
SELECT same('rank', 'SELECT g, id, k, rank() OVER (PARTITION BY g ORDER BY k) AS r FROM @T@ WHERE s <> '''' AND id < 800');
SELECT same('running sum', 'SELECT g, id, sum(k) OVER (PARTITION BY g ORDER BY id) AS r FROM @T@ WHERE s LIKE ''p%'' AND id < 800');
SELECT same('lag', 'SELECT g, id, lag(k) OVER (PARTITION BY g ORDER BY id) AS r FROM @T@ WHERE v = ''v3'' AND id < 800');

\echo == after updates and deletes
UPDATE wt SET s = 'p3' WHERE id % 53 = 0;
UPDATE wh SET s = 'p3' WHERE id % 53 = 0;
DELETE FROM wt WHERE id % 41 = 0;
DELETE FROM wh WHERE id % 41 = 0;
SELECT same('text = after dml', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s = ''p3'') w WHERE rn <= 3');
SELECT same('text <> after dml', 'SELECT * FROM (SELECT g, id, row_number() OVER (PARTITION BY g ORDER BY id) AS rn FROM @T@ WHERE s <> '''') w WHERE rn <= 3');

DROP TABLE wt, wh;
DROP VIEW wbulk;
DROP FUNCTION plan_of(text), text_of(text), same(text, text);
DROP EXTENSION pg_sift;
