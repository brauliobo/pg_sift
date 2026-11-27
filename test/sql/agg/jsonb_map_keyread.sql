-- The members of a map `col.* float8` of a sift_col index of format major 2 that a statement reads by a constant key (col -> 'k', col['k'], their casts to float8 and to an integer, (col ->> 'k')::float8),
-- and a subscript of a key the index keeps as a leaf: the aggregate scan of SiftAggV6 finds the member by its key in each row of the table, its value or NULL.
-- Every query is run with the node and without it (sift.enable_agg) and the two answers are compared, errors included; the first word says they agree, the second that the plan is the one the query
-- expects (node: SiftAggV6; node18: SiftAggV6 where PostgreSQL gives NULL for the cast of a JSON null, the normal plan before). The plan comes from the cost of the pages, not from a setting that forces it.
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

CREATE TABLE mk (id int PRIMARY KEY, g int, attributes jsonb, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mk SELECT i, i % 7,
	CASE i % 9 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN '{"channel": "web", "device": "x"}' WHEN 3 THEN '{"channel": "app"}' ELSE jsonb_build_object('channel', CASE i % 4 WHEN 0 THEN 'web' WHEN 1 THEN 'pos' ELSE 'app' END) END,
	CASE i % 10
		WHEN 0 THEN NULL
		WHEN 1 THEN '{}'
		WHEN 2 THEN jsonb_build_object('m_total', i * 0.5, 'quality', i % 6)
		WHEN 3 THEN jsonb_build_object('m_total', -i * 0.25, 'quality', i % 5 + 0.5)
		WHEN 4 THEN jsonb_build_object('quality', i % 7)
		WHEN 5 THEN jsonb_build_object('m_total', i, 'quality', 4.5)
		WHEN 6 THEN jsonb_build_object('m_total', 2.5, 'quality', -0.5)
		WHEN 7 THEN jsonb_build_object('m_total', -2.5, 'quality', 3.5)
		WHEN 8 THEN jsonb_build_object('other', 1)
		ELSE jsonb_build_object('m_total', i * 1.0, 'quality', 4, 'big', 30000000000)
	END
FROM generate_series(1, 3000) i;
CREATE INDEX mk_col ON mk USING sift_col (id, g, attributes, metrics) WITH (paths = 'attributes->channel text, metrics.* float8');
ANALYZE mk;
-- rows the index has not seen yet are read from the table
INSERT INTO mk SELECT 3000 + i, i % 7, jsonb_build_object('channel', CASE i % 3 WHEN 0 THEN 'web' ELSE 'app' END), CASE i % 4 WHEN 0 THEN NULL ELSE jsonb_build_object('m_total', i + 0.5, 'quality', i % 6) END
FROM generate_series(1, 200) i;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text := ''; l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	RETURN plan;
END $$;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'error ' || SQLSTATE || ' ' || SQLERRM;
END $$;

CREATE FUNCTION same(q text, want text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text; node boolean; expect_node boolean := want = 'node' OR (want = 'node18' AND current_setting('server_version_num')::int >= 180000);
BEGIN
	SET LOCAL sift.enable_agg = on;
	plan := plan_of(q);
	a := answer(q);
	SET LOCAL sift.enable_agg = off;
	b := answer(q);
	node := plan LIKE '%SiftAggV6%';
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN node = expect_node THEN want ELSE 'UNEXPECTED ' || plan END) || ' ' ||
		(CASE WHEN a LIKE 'error %' THEN a ELSE 'ok' END) || ' ' || q;
END $$;

-- Q-01. The plans: the members the statement names are columns of the scan of the index
EXPLAIN (COSTS OFF) SELECT attributes['channel'] #>> '{}', avg((metrics['m_total'])::float8) FROM mk GROUP BY 1;
EXPLAIN (COSTS OFF) SELECT (metrics -> 'm_total')::int, count(*) FROM mk WHERE (metrics -> 'quality')::int >= 4 GROUP BY 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM mk WHERE (metrics -> 'quality')::bigint >= 4 AND g < 3;

-- Q-02. The values: the subscript of a key that is a leaf as its text, the member as a float8, and as an integer
SELECT attributes['channel'] #>> '{}' AS channel, count(*) AS n, round(avg((metrics['m_total'])::float8)::numeric, 3) AS m_total FROM mk GROUP BY 1 ORDER BY 1 NULLS FIRST;
SELECT (metrics -> 'quality')::int AS q, count(*) AS n FROM mk GROUP BY 1 ORDER BY 1 NULLS FIRST;
SELECT (metrics -> 'm_total')::int AS score, count(*) AS n FROM mk WHERE (metrics -> 'quality')::int >= 4 GROUP BY 1 ORDER BY 1 NULLS FIRST;

-- Q-03. The subscript of a document and its forms: #>> '{}' and ->> are one text, a path of one key is the key, and the float8 of the text of a member
SELECT same(q, w) FROM (VALUES
	('SELECT attributes[''channel''] #>> ''{}'', count(*) FROM mk GROUP BY 1', 'node'),
	('SELECT attributes ->> ''channel'', attributes[''channel''] #>> ''{}'', count(*) FROM mk GROUP BY 1, 2', 'node'),
	('SELECT attributes #>> ''{channel}'', count(*) FROM mk GROUP BY 1', 'node'),
	('SELECT attributes[''channel''] #>> ''{}'', avg((metrics[''m_total''])::float8) FROM mk GROUP BY 1', 'node'),
	('SELECT attributes ->> ''channel'', avg((metrics ->> ''m_total'')::float8) FROM mk GROUP BY 1', 'node'),
	('SELECT attributes ->> ''channel'', avg((metrics #>> ''{m_total}'')::float8) FROM mk GROUP BY 1', 'node'),
	('SELECT avg((metrics -> ''m_total'')::float8), avg((metrics #> ''{m_total}'')::float8), avg(jsonb_extract_path(metrics, ''m_total'')::float8) FROM mk', 'node18'),
	('SELECT g, sum((metrics[''m_total''])::float8), min((metrics[''m_total''])::float8), max((metrics[''m_total''])::float8), count((metrics[''m_total''])::float8), count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT count(*), count((metrics[''nokey''])::float8), sum((metrics[''nokey''])::float8) FROM mk', 'node18'),
	('SELECT attributes[''channel''] #>> ''{}'', count(*) FILTER (WHERE (metrics[''m_total''])::float8 >= 2) FROM mk GROUP BY 1', 'node18'),
	('SELECT g, count(*) FROM mk WHERE (metrics[''m_total''])::float8 >= 2 AND g < 5 GROUP BY 1', 'node18'),
	('SELECT g, count(*) FROM mk WHERE (metrics[''m_total''])::float8 BETWEEN -1 AND 100 OR (metrics[''quality''])::float8 = 4 GROUP BY 1', 'normal'),
	('SELECT g, count(*) FROM mk WHERE (metrics[''m_total''])::float8 IS NULL GROUP BY 1', 'node18'),
	('SELECT g, sum((metrics[''m_total''])::float8) FROM mk GROUP BY 1 HAVING sum((metrics[''m_total''])::float8) > 100', 'node18')
) t(q, w);

-- Q-04. What the scan does not take keeps the normal plan and the same answers: a chained subscript, an integer subscript, the jsonb value as it is, its text, a key of the leaf kind
SELECT same(q, w) FROM (VALUES
	('SELECT attributes[''channel''][''x''] #>> ''{}'', count(*) FROM mk GROUP BY 1', 'normal'),
	('SELECT (metrics[0])::float8, count(*) FROM mk GROUP BY 1', 'normal'),
	('SELECT metrics[''m_total''], count(*) FROM mk WHERE g = 1 GROUP BY 1', 'normal'),
	('SELECT metrics ->> ''m_total'', count(*) FROM mk WHERE g = 1 GROUP BY 1', 'normal'),
	('SELECT g, count(*) FROM mk WHERE metrics[''m_total''] = ''2.5'' GROUP BY 1', 'normal'),
	('SELECT (metrics -> ''m_total'')::numeric, count(*) FROM mk WHERE g = 1 GROUP BY 1', 'normal'),
	('SELECT (metrics -> ''m_total'')::float4, count(*) FROM mk WHERE g = 1 GROUP BY 1', 'normal'),
	('SELECT (metrics ->> ''m_total'')::int, count(*) FROM mk WHERE g = 1 GROUP BY 1', 'normal')
) t(q, w);

-- Q-05. The integer cast of a member: a group key, a filter, an aggregate and its argument, as a smallint, an integer and a bigint; numeric rounds half away from zero
SELECT same(q, w) FROM (VALUES
	('SELECT (metrics -> ''m_total'')::int, count(*) FROM mk WHERE (metrics -> ''quality'')::int >= 4 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::int, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::bigint, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::smallint, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT g, sum((metrics -> ''quality'')::int), min((metrics -> ''m_total'')::bigint), max((metrics -> ''m_total'')::int), avg((metrics -> ''quality'')::int) FROM mk GROUP BY 1', 'node18'),
	('SELECT count(*) FILTER (WHERE (metrics -> ''quality'')::int >= 4), count(*) FILTER (WHERE (metrics -> ''quality'')::bigint = 5), count(*) FILTER (WHERE g = 3) FROM mk', 'node18'),
	('SELECT count(*) FILTER (WHERE (metrics -> ''quality'')::bigint = 5 OR g = 3) FROM mk', 'node18'),
	('SELECT g, count(*) FROM mk WHERE (metrics -> ''quality'')::smallint IN (1, 2, 5) GROUP BY 1', 'node18'),
	('SELECT g, count(*) FROM mk WHERE (metrics -> ''quality'')::int IS NULL GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::int, (metrics -> ''m_total'')::int, count(*) FROM mk WHERE g < 3 GROUP BY 1, 2', 'node18'),
	('SELECT CAST(jsonb_extract_path(metrics, ''quality'') AS integer), count(*) FROM mk WHERE CAST(jsonb_extract_path(metrics, ''m_total'') AS bigint) >= 3 GROUP BY 1', 'node18'),
	('SELECT (metrics[''quality''])::int, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::int AS q, count(*) FROM mk WHERE g < 3 GROUP BY 1 HAVING count(*) > 10', 'node18'),
	('SELECT count(*) FROM mk WHERE (metrics -> ''quality'')::int >= 4 AND g < 3', 'node18'),
	('SELECT (metrics -> ''quality'')::int, (metrics ->> ''quality'')::float8, count(*) FROM mk GROUP BY 1, 2', 'node18')
) t(q, w);

-- Q-06. A member the integer cannot hold raises where the row reaches the cast, as the cast of the table does: the rows that do not reach it do not raise
SELECT same(q, w) FROM (VALUES
	('SELECT (metrics -> ''big'')::int, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''big'')::bigint, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''big'')::smallint, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''m_total'')::smallint, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT sum((metrics -> ''big'')::bigint), max((metrics -> ''big'')::bigint) FROM mk', 'node18'),
	('SELECT count(*) FROM mk WHERE (metrics -> ''big'')::int > 1 AND g = 99', 'node18'),
	('SELECT (metrics -> ''big'')::int, count(*) FROM mk WHERE g = 99 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''big'')::int, count(*) FROM mk WHERE id < 9 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''big'')::int, count(*) FROM mk WHERE (metrics -> ''quality'')::int >= 5 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''big'')::int, count(*) FROM mk WHERE (metrics -> ''quality'')::int >= 4 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::int, count(*) FILTER (WHERE (metrics -> ''big'')::int > 0) FROM mk WHERE (metrics -> ''quality'')::int >= 5 GROUP BY 1', 'node18'),
	-- two conditions that cast: PostgreSQL casts the second for the rows the first keeps, which the batch quals of the scan do not follow: the normal plan
	('SELECT count(*) FROM mk WHERE (metrics -> ''quality'')::int >= 5 AND (metrics -> ''big'')::int > 0', 'plain'),
	('SELECT count(*) FROM mk WHERE (metrics -> ''big'')::int > 0 AND (metrics -> ''quality'')::int >= 5', 'plain')
) t(q, w);

-- Q-07. A JSON null member is NULL for the casts of PostgreSQL 18 (the text and its float8 are NULL on every version), and a member that is no number is read from the table as it reads it
CREATE TABLE mn (id int PRIMARY KEY, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mn SELECT i, CASE WHEN i % 7 = 0 THEN NULL WHEN i % 5 = 0 THEN jsonb_build_object('a', i, 'tip', NULL) ELSE jsonb_build_object('a', i, 'tip', i % 3) END FROM generate_series(1, 600) i;
CREATE INDEX mn_col ON mn USING sift_col (id, metrics) WITH (paths = 'metrics.* float8');
ANALYZE mn;
SELECT same(q, w) FROM (VALUES
	('SELECT count((metrics ->> ''tip'')::float8), avg((metrics ->> ''tip'')::float8) FROM mn', 'node'),
	('SELECT (metrics -> ''tip'')::int, count(*) FROM mn GROUP BY 1', 'node18'),
	('SELECT count((metrics -> ''tip'')::float8), sum((metrics -> ''tip'')::bigint) FROM mn', 'node18'),
	('SELECT count(*) FROM mn WHERE (metrics -> ''tip'')::int IS NULL', 'node18')
) t(q, w);

CREATE TABLE md (id int PRIMARY KEY, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO md SELECT i, CASE i % 6 WHEN 0 THEN '{"a": "5", "b": 1}' WHEN 1 THEN '{"a": 2.5, "b": "7.5"}' WHEN 2 THEN '{"a": 1}' WHEN 3 THEN NULL WHEN 4 THEN '{"a": true, "b": {"c": 1}}' ELSE jsonb_build_object('a', i, 'b', i % 4) END FROM generate_series(1, 600) i;
CREATE INDEX md_col ON md USING sift_col (id, metrics) WITH (paths = 'metrics.* float8');
ANALYZE md;
SELECT same(q, w) FROM (VALUES
	('SELECT (metrics -> ''a'')::int, count(*) FROM md GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''a'')::float8, count(*) FROM md GROUP BY 1', 'node18'),
	('SELECT (metrics ->> ''a'')::float8, count(*) FROM md GROUP BY 1', 'node'),
	('SELECT (metrics ->> ''b'')::float8, count(*) FROM md WHERE id IN (1, 5, 7, 11) GROUP BY 1', 'node'),
	('SELECT (metrics -> ''a'')::int, count(*) FROM md WHERE id IN (2, 3, 5, 8, 9, 11) GROUP BY 1', 'node18'),
	('SELECT sum((metrics -> ''b'')::bigint) FROM md WHERE id IN (5, 11, 17)', 'node18'),
	('SELECT sum((metrics -> ''b'')::bigint) FROM md WHERE id IN (1, 7)', 'node18'),
	('SELECT count(*) FROM md WHERE (metrics -> ''a'')::int >= 2 AND id IN (2, 8, 14)', 'node18')
) t(q, w);

-- Q-08. A key of a document the leaf cannot hold (a number, a boolean, an object, an array, a JSON null) beside a member read by key: the rows are read from the table, and the text of the key is as `->>` gives it
CREATE TABLE mx (id int PRIMARY KEY, attributes jsonb, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mx SELECT i,
	CASE i % 8 WHEN 0 THEN NULL WHEN 1 THEN '{"channel": 5}'::jsonb WHEN 2 THEN '{"channel": true}' WHEN 3 THEN '{"channel": {"a": 1}}' WHEN 4 THEN '{"channel": [1]}' WHEN 5 THEN '{"channel": null}' WHEN 6 THEN '{}' ELSE '{"channel": "web"}' END,
	jsonb_build_object('m_total', i % 5 + 0.5, 'quality', i % 7)
FROM generate_series(1, 400) i;
CREATE INDEX mx_col ON mx USING sift_col (id, attributes, metrics) WITH (paths = 'attributes->channel text, metrics.* float8');
ANALYZE mx;
SELECT same(q, w) FROM (VALUES
	('SELECT attributes[''channel''] #>> ''{}'', count(*), sum((metrics[''m_total''])::float8) FROM mx GROUP BY 1', 'node18'),
	('SELECT attributes ->> ''channel'', (metrics -> ''quality'')::int, count(*) FROM mx GROUP BY 1, 2', 'node18'),
	('SELECT attributes ->> ''channel'', avg((metrics ->> ''m_total'')::float8) FROM mx WHERE id < 200 GROUP BY 1', 'node')
) t(q, w);

-- Q-09. Rows that change: an update, a delete, and a map that is no object
UPDATE mk SET metrics = jsonb_build_object('m_total', 100, 'quality', 5) WHERE id % 50 = 3;
DELETE FROM mk WHERE id % 60 = 4;
SELECT same(q, w) FROM (VALUES
	('SELECT (metrics -> ''quality'')::int, count(*), sum((metrics[''m_total''])::float8) FROM mk GROUP BY 1', 'node18'),
	('SELECT attributes[''channel''] #>> ''{}'', count(*), avg((metrics ->> ''m_total'')::float8) FROM mk GROUP BY 1', 'node')
) t(q, w);
INSERT INTO mk VALUES (9001, 1, '{}', '[1, 2]'), (9002, 2, '{}', '"s"'), (9003, 3, '{}', 'null'), (9004, 4, '{}', '3');
SELECT same(q, w) FROM (VALUES
	('SELECT (metrics -> ''quality'')::int, count(*) FROM mk GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::int, count(*) FROM mk WHERE id < 9000 GROUP BY 1', 'node18'),
	('SELECT sum((metrics[''m_total''])::float8) FROM mk WHERE id IN (9001, 9002, 9003, 9004)', 'node18'),
	('SELECT sum((metrics ->> ''m_total'')::float8) FROM mk WHERE id IN (9003, 9004)', 'node'),
	('SELECT sum((metrics #>> ''{0}'')::float8) FROM mk WHERE id IN (9001, 9004)', 'normal'),
	('SELECT sum((metrics #>> ''{m_total}'')::float8) FROM mk WHERE id IN (9001, 9003, 9004)', 'node')
) t(q, w);

-- Q-10. A key the index keeps as a leaf of float8 (no map): the same casts, with the rows whose value is another JSON type, or out of range, read from the table
CREATE TABLE ms (id int PRIMARY KEY, g int, k int, attributes jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ms SELECT i, i % 5, i % 12,
	CASE i % 12 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN '{"score": null}' WHEN 3 THEN '{"score": "4"}' WHEN 4 THEN '{"score": 2.5, "channel": "a"}' WHEN 5 THEN '{"score": -2.5, "channel": "b"}'
		WHEN 6 THEN '{"score": true}' WHEN 7 THEN '{"score": 3, "channel": "a"}' WHEN 8 THEN '{"score": 1e400}' ELSE jsonb_build_object('score', i % 9 + 0.25, 'channel', 'c') END
FROM generate_series(1, 1200) i;
CREATE INDEX ms_col ON ms USING sift_col (id, g, k, attributes) WITH (paths = 'attributes->score float8, attributes->channel text');
ANALYZE ms;
INSERT INTO ms SELECT 1200 + i, i % 5, 100, jsonb_build_object('score', i % 7 - 3.5, 'channel', 'd') FROM generate_series(1, 100) i;
SELECT same(q, w) FROM (VALUES
	('SELECT (attributes -> ''score'')::int, count(*) FROM ms WHERE k IN (0, 1, 2, 4, 5, 7, 9, 10, 11, 100) GROUP BY 1', 'node18'),
	('SELECT (attributes -> ''score'')::int, count(*) FROM ms GROUP BY 1', 'node18'),
	('SELECT (attributes -> ''score'')::float8, count(*) FROM ms WHERE k IN (0, 1, 2, 4, 5, 7, 9, 10, 11, 100) GROUP BY 1', 'node18'),
	('SELECT g, sum((attributes[''score''])::bigint), avg((attributes -> ''score'')::int) FROM ms WHERE k IN (0, 1, 2, 4, 5, 7, 9, 10, 11, 100) GROUP BY 1', 'node18'),
	('SELECT attributes ->> ''channel'', count(*) FILTER (WHERE (attributes -> ''score'')::int >= 3) FROM ms WHERE k IN (0, 1, 2, 4, 5, 7, 9, 10, 11, 100) GROUP BY 1', 'node18'),
	-- PostgreSQL tests the cheaper cast before the list, for every row, where the scan keys of the list drop rows first: the normal plan
	('SELECT count(*) FROM ms WHERE (attributes -> ''score'')::int IS NULL AND k IN (0, 1, 2, 4, 5, 7, 9, 10, 11, 100)', 'plain'),
	('SELECT (attributes -> ''score'')::int, count(*) FROM ms WHERE k IN (3, 4) GROUP BY 1', 'node18'),
	('SELECT (attributes -> ''score'')::smallint, count(*) FROM ms WHERE k = 8 GROUP BY 1', 'node18'),
	('SELECT (attributes -> ''score'')::int, count(*) FROM ms WHERE k = 6 GROUP BY 1', 'node18')
) t(q, w);

-- Q-11. The `->>` text of a boolean key beside a member read by key: the leaf holds booleans, the key is their text ('true', 'false'), before and after the rows change
CREATE TABLE mb (id int PRIMARY KEY, attributes jsonb, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mb SELECT i, CASE i % 10 WHEN 0 THEN '{"channel": "web"}'::jsonb WHEN 1 THEN NULL ELSE jsonb_build_object('channel', CASE i % 3 WHEN 0 THEN 'web' ELSE 'app' END, 'normalized', i % 10 < 7) END,
	jsonb_build_object('m_total', i % 9 + 0.5)
FROM generate_series(1, 3000) i;
CREATE INDEX mb_col ON mb USING sift_col (id, attributes, metrics) WITH (paths = 'attributes->channel text, attributes->normalized bool, metrics.* float8');
ANALYZE mb;
SELECT attributes ->> 'channel' AS channel, attributes ->> 'normalized' AS normalized, count(*) AS n, avg((metrics ->> 'm_total')::float8) AS m_total FROM mb GROUP BY 1, 2 ORDER BY 1, 2 NULLS FIRST;
SELECT same(q, w) FROM (VALUES
	('SELECT attributes ->> ''channel'', attributes ->> ''normalized'', count(*), avg((metrics ->> ''m_total'')::float8) FROM mb GROUP BY 1, 2', 'node'),
	('SELECT attributes ->> ''normalized'', sum((metrics[''m_total''])::float8) FROM mb GROUP BY 1', 'node')
) t(q, w);
UPDATE mb SET attributes = attributes || '{"normalized": false}' WHERE id % 50 = 3;
UPDATE mb SET attributes = attributes || '{"normalized": "yes"}' WHERE id % 70 = 4;
INSERT INTO mb SELECT 3000 + i, jsonb_build_object('channel', 'pos', 'normalized', i % 2 = 0), jsonb_build_object('m_total', i) FROM generate_series(1, 200) i;
SELECT same(q, w) FROM (VALUES
	('SELECT attributes ->> ''channel'', attributes ->> ''normalized'', count(*), avg((metrics ->> ''m_total'')::float8) FROM mb GROUP BY 1, 2', 'node'),
	('SELECT attributes ->> ''normalized'', sum((metrics[''m_total''])::float8) FROM mb GROUP BY 1', 'node')
) t(q, w);
VACUUM mb;
SELECT same(q, w) FROM (VALUES
	('SELECT attributes ->> ''channel'', attributes ->> ''normalized'', count(*), avg((metrics ->> ''m_total'')::float8) FROM mb GROUP BY 1, 2', 'node')
) t(q, w);

-- Q-12. A key longer than the index keeps of a map (255 bytes) has no leaf: a row whose member the other keys do not name is read again from the table and answered from its
-- document, before and after the rows change (the slab and the parts)
CREATE TABLE ml (id int PRIMARY KEY, g int, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ml SELECT i, i % 4, CASE i % 5 WHEN 0 THEN jsonb_build_object(repeat('k', 300), 1, 'm_total', i % 7 + 0.5) WHEN 1 THEN jsonb_build_object(repeat('z', 300), 2)
	WHEN 2 THEN jsonb_build_object(repeat('y', 300), 'm_total') ELSE jsonb_build_object('m_total', i % 3, 'quality', 1) END
FROM generate_series(1, 3000) i;
CREATE INDEX ml_col ON ml USING sift_col (id, g, metrics) WITH (paths = 'metrics.* float8');
ANALYZE ml;
CREATE FUNCTION reread(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Read Again%' THEN RETURN btrim(l); END IF;
	END LOOP;
	RETURN NULL;
END $$;
SELECT same(q, w), reread(q) FROM (VALUES
	('SELECT g, count((metrics ->> ''m_total'')::float8), sum((metrics ->> ''m_total'')::float8) FROM ml GROUP BY 1', 'node'),
	('SELECT (metrics ->> ''m_total'')::float8 AS t, count(*) FROM ml WHERE g < 2 GROUP BY 1', 'node')
) t(q, w);
INSERT INTO ml SELECT 3000 + i, i % 4, jsonb_build_object(repeat('w', 300), i, 'quality', 2) FROM generate_series(1, 40) i;
SELECT same(q, w) FROM (VALUES
	('SELECT g, count((metrics ->> ''m_total'')::float8), sum((metrics ->> ''m_total'')::float8) FROM ml GROUP BY 1', 'node')
) t(q, w);

-- Q-13. A condition and a read of the rows it keeps that cast to two types raise two errors: of the first row that raises, the one PostgreSQL meets first in it (the condition, then
-- the read), whichever cast comes first in the scan
CREATE TABLE mo (id int PRIMARY KEY, metrics jsonb) WITH (autovacuum_enabled = false);
INSERT INTO mo SELECT i, CASE i WHEN 10 THEN '{"m_total": 5, "quality": "q"}' WHEN 20 THEN '{"m_total": "t", "quality": 1}' WHEN 30 THEN '{"m_total": "u", "quality": "v"}'
	ELSE jsonb_build_object('m_total', i % 9, 'quality', i % 4) END::jsonb FROM generate_series(1, 2000) i;
CREATE INDEX mo_col ON mo USING sift_col (id, metrics) WITH (paths = 'metrics.* float8');
VACUUM ANALYZE mo;
SELECT same(q, w) FROM (VALUES
	('SELECT (metrics -> ''quality'')::bigint, count(*) FROM mo WHERE (metrics -> ''m_total'')::int >= 0 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::bigint, count(*) FROM mo WHERE (metrics -> ''m_total'')::int >= 0 AND id <> 10 GROUP BY 1', 'node18'),
	('SELECT (metrics -> ''quality'')::bigint, count(*) FROM mo WHERE (metrics -> ''m_total'')::int >= 0 AND id > 25 GROUP BY 1', 'node18'),
	('SELECT count(*) FILTER (WHERE (metrics -> ''quality'')::bigint > 1), count(*) FROM mo WHERE (metrics -> ''m_total'')::int >= 0', 'node18')
) t(q, w);

-- Q-14. A star join that drops fact rows: the cast of a member raises for a row that reaches the groups through the join, and not for a row the join drops
CREATE TABLE mt (id int PRIMARY KEY, kind text);
INSERT INTO mt SELECT i, 'k' || i % 3 FROM generate_series(1, 25) i;
ANALYZE mt;
SELECT same(q, w) FROM (VALUES
	('SELECT t.kind, count(*), sum((o.metrics ->> ''m_total'')::float8) FROM mt t JOIN mo o ON o.id = t.id GROUP BY 1', 'node'),
	('SELECT t.kind, count(*), sum((o.metrics ->> ''m_total'')::float8) FROM mt t JOIN mo o ON o.id = t.id + 100 GROUP BY 1', 'node'),
	('SELECT t.kind, count(o.id), sum((o.metrics ->> ''m_total'')::float8) FROM mt t LEFT JOIN mo o ON o.id = t.id + 100 GROUP BY 1', 'node')
) t(q, w);

DROP FUNCTION same(text, text), answer(text), plan_of(text), reread(text);
DROP TABLE mk, mn, md, mx, ms, mb, ml, mo, mt;
DROP EXTENSION pg_sift;
