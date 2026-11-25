-- The text quals of a sift_table (=, <>, <, >=, IN, LIKE, NOT LIKE, BETWEEN, a column against a column, OR of them) are quals of the SiftAggV6 scan, like those of an index: the scan decodes the text
-- cell of the table as it does the key of a group, and the quals run over the batch. Every check runs the statement on the table and on a heap twin holding the same rows, compares the rows as text,
-- and asserts the node that read the table (SiftAggV6 over the table, never a Seq Scan).
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

CREATE TABLE qt (id int PRIMARY KEY, g int, s text, v varchar(20), c text COLLATE "C") USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE qh (LIKE qt INCLUDING ALL);

CREATE VIEW qbulk AS SELECT g AS id, g % 7 AS g,
	CASE g % 11 WHEN 0 THEN NULL WHEN 1 THEN '' WHEN 2 THEN 'google.com/' || (g % 13) WHEN 3 THEN 'Google' WHEN 4 THEN 'a%b' WHEN 5 THEN 'ab' || repeat('c', g % 40)
		WHEN 6 THEN 'é' || (g % 5) WHEN 7 THEN 'x' || (g % 3) ELSE 'p' || (g % 17) END AS s,
	CASE WHEN g % 9 = 0 THEN NULL WHEN g % 9 = 1 THEN '' ELSE 'v' || (g % 6) END AS v,
	CASE WHEN g % 8 = 0 THEN NULL WHEN g % 8 = 1 THEN '' ELSE chr(65 + g % 20) || chr(97 + g % 7) END AS c
FROM generate_series(1, 6000) g;

CREATE VIEW qedge AS
SELECT 100000 + i AS id, 9 AS g, s, v, c
FROM (VALUES
	(1, ' ', ' ', ' '),
	(2, 'google', 'google', 'google'),
	(3, '%google%', 'a_c', 'a_c'),
	(4, repeat('long', 500), repeat('w', 20), repeat('long', 500)),
	(5, 'ß', 'ß', 'ß'),
	(6, 'p1 ', 'v1 ', 'p1 '),
	(7, E'tab\there', E'a\\b', E'a\\b'),
	(8, '日本語', '日本', '日本語')
) AS e(i, s, v, c);

INSERT INTO qt SELECT * FROM qbulk UNION ALL SELECT * FROM qedge;
INSERT INTO qh SELECT * FROM qbulk UNION ALL SELECT * FROM qedge;
SELECT sift.table_maintain('qt') IS NOT NULL AS maintained;
ANALYZE qt;
ANALYZE qh;

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
DECLARE plan text := plan_of(replace(q, '@T@', 'qt'));
BEGIN
	IF text_of(replace(q, '@T@', 'qt')) IS DISTINCT FROM text_of(replace(q, '@T@', 'qh')) THEN
		RAISE EXCEPTION 'FAILED %: the rows of the table are not those of the heap twin', name;
	END IF;
	IF plan NOT LIKE '%Custom Scan (SiftAggV6)%Table: qt%' OR plan LIKE '%Seq Scan%' THEN RAISE EXCEPTION 'FAILED %: SiftAggV6 did not read the table: %', name, plan; END IF;
	RETURN name || ': ok';
END $$;

\echo == one text qual and a count
SELECT same('s =', 'SELECT count(*) FROM @T@ WHERE s = ''p3''');
SELECT same('s = empty', 'SELECT count(*) FROM @T@ WHERE s = ''''');
SELECT same('s <>', 'SELECT count(*) FROM @T@ WHERE s <> ''''');
SELECT same('s <> const', 'SELECT count(*), sum(g) FROM @T@ WHERE s <> ''google.com/3''');
SELECT same('s <', 'SELECT count(*) FROM @T@ WHERE s < ''b''');
SELECT same('s >=', 'SELECT count(*), min(id) FROM @T@ WHERE s >= ''google''');
SELECT same('s between', 'SELECT count(*) FROM @T@ WHERE s BETWEEN ''a'' AND ''p5''');
SELECT same('s in', 'SELECT count(*) FROM @T@ WHERE s IN (''p3'', ''Google'', ''ß'', '''')');
SELECT same('s not in', 'SELECT count(*) FROM @T@ WHERE s NOT IN (''p3'', ''Google'', '''')');
SELECT same('s like infix', 'SELECT count(*) FROM @T@ WHERE s LIKE ''%google%''');
SELECT same('s not like infix', 'SELECT count(*) FROM @T@ WHERE s NOT LIKE ''%google%''');
SELECT same('s like prefix', 'SELECT count(*) FROM @T@ WHERE s LIKE ''ab%''');
SELECT same('s like underscore', 'SELECT count(*) FROM @T@ WHERE s LIKE ''p_''');
SELECT same('s like percent literal', 'SELECT count(*) FROM @T@ WHERE s LIKE ''a\%b''');
SELECT same('s ilike', 'SELECT count(*) FROM @T@ WHERE s ILIKE ''%GOOGLE%''');
SELECT same('s is null', 'SELECT count(*) FROM @T@ WHERE s IS NULL');
SELECT same('s is not null', 'SELECT count(*) FROM @T@ WHERE s IS NOT NULL AND s <> ''''');
SELECT same('v varchar =', 'SELECT count(*) FROM @T@ WHERE v = ''v3''');
SELECT same('v varchar <>', 'SELECT count(*) FROM @T@ WHERE v <> ''''');
SELECT same('v varchar like', 'SELECT count(*) FROM @T@ WHERE v LIKE ''v%''');
SELECT same('c collate C <', 'SELECT count(*) FROM @T@ WHERE c < ''Cb''');
SELECT same('c collate C like', 'SELECT count(*) FROM @T@ WHERE c LIKE ''%a%''');

\echo == several quals, a column against a column, OR
SELECT same('and of three', 'SELECT count(*) FROM @T@ WHERE s <> '''' AND s NOT LIKE ''%google%'' AND g > 2');
SELECT same('or', 'SELECT count(*) FROM @T@ WHERE s = ''p3'' OR v = ''v2''');
SELECT same('or like', 'SELECT count(*) FROM @T@ WHERE s LIKE ''%google%'' OR s LIKE ''ab%''');
SELECT same('column against column', 'SELECT count(*) FROM @T@ WHERE s = c');
SELECT same('column against column <>', 'SELECT count(*) FROM @T@ WHERE s <> v');
SELECT same('length of a filtered text', 'SELECT sum(length(s)) FROM @T@ WHERE s <> ''''');

\echo == the group by of the ClickBench filters
SELECT same('group by text, text <>', 'SELECT s, count(*) FROM @T@ WHERE s <> '''' GROUP BY s');
SELECT same('group by int, text <>', 'SELECT g, count(*), sum(id) FROM @T@ WHERE s <> '''' GROUP BY g');
SELECT same('group by text, like', 'SELECT s, count(*), min(id) FROM @T@ WHERE s LIKE ''%google%'' GROUP BY s');
SELECT same('group by two, text <>', 'SELECT g, v, count(*), avg(id) FROM @T@ WHERE s <> '''' AND v <> '''' GROUP BY g, v');
SELECT same('count distinct, text <>', 'SELECT g, count(DISTINCT s) FROM @T@ WHERE v <> '''' GROUP BY g');
SELECT same('having', 'SELECT s, count(*) FROM @T@ WHERE v <> '''' GROUP BY s HAVING count(*) > 20');
SELECT same('top n', 'SELECT s, count(*) AS n FROM @T@ WHERE s <> '''' GROUP BY s ORDER BY n DESC, s LIMIT 5');
SELECT same('filter clause', 'SELECT count(*) FILTER (WHERE s <> ''''), count(*) FILTER (WHERE s LIKE ''%google%'') FROM @T@');

\echo == the rows of an ORDER BY .. LIMIT with a text filter
SELECT same('top n rows, text <>', 'SELECT s, id FROM @T@ WHERE s <> '''' ORDER BY g, id LIMIT 10');
SELECT same('top n rows, like', 'SELECT * FROM @T@ WHERE s LIKE ''%google%'' ORDER BY g DESC, id LIMIT 12');
SELECT same('top n rows, text key', 'SELECT s FROM @T@ WHERE s <> '''' AND v <> '''' ORDER BY s, id LIMIT 15');
SELECT same('top n rows, text in', 'SELECT id, v FROM @T@ WHERE s IN (''p3'', ''Google'') ORDER BY id DESC LIMIT 9 OFFSET 3');

\echo == after updates, deletes and a table rewrite
UPDATE qt SET s = 'p3' WHERE id % 97 = 0;
UPDATE qh SET s = 'p3' WHERE id % 97 = 0;
DELETE FROM qt WHERE id % 89 = 0;
DELETE FROM qh WHERE id % 89 = 0;
SELECT same('s = after dml', 'SELECT count(*) FROM @T@ WHERE s = ''p3''');
SELECT same('s <> after dml', 'SELECT s, count(*) FROM @T@ WHERE s <> '''' GROUP BY s');
SELECT same('s like after dml', 'SELECT count(*) FROM @T@ WHERE s LIKE ''%google%''');
VACUUM FULL qt;
VACUUM FULL qh;
SELECT same('s <> after rewrite', 'SELECT g, count(*) FROM @T@ WHERE s <> '''' GROUP BY g');
SELECT same('s like after rewrite', 'SELECT count(*) FROM @T@ WHERE s NOT LIKE ''%google%''');

\echo == parameters
PREPARE pt(text) AS SELECT count(*) FROM qt WHERE s = $1 OR s LIKE $1;
PREPARE ph(text) AS SELECT count(*) FROM qh WHERE s = $1 OR s LIKE $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE pt('p3');
EXECUTE ph('p3');
EXECUTE pt('ab%');
EXECUTE ph('ab%');
RESET plan_cache_mode;

DROP TABLE qt, qh;
DROP VIEW qbulk, qedge;
DROP FUNCTION plan_of(text), text_of(text), same(text, text);
DROP EXTENSION pg_sift;
