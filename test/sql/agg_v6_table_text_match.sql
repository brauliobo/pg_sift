-- A text query (body @@> 'q') among the quals of an aggregate of a sift_table with a sift text index: the scan of the table reads the rows of the hits only, the hits of the query going to it in heap
-- order as the chunks of a bridge (agg/v6/v6_text.c), as they go to the scan of a sift_col index. Every check compares the statement with the same statement that carries a volatile qual (which
-- keeps the text query out of the scan) and asserts the node that read the table and, where the text intake is the point, the text index it names. A scan of an unnest or a star join takes no
-- text intake: those statements are not planned on SiftAggV6. A text query that hits no row or a few rows is left to the text index's own scan by the cost model.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
\pset format unaligned
SET max_parallel_workers_per_gather = 0;
SET effective_cache_size = '8kB';
SET sift.enable_scan = off;

CREATE TABLE tm (id int PRIMARY KEY, tenant int NOT NULL, region int NOT NULL, amount int NOT NULL, tag text NOT NULL, body text NOT NULL, arr int[]) USING sift_table
	WITH (autovacuum_enabled = false);
INSERT INTO tm SELECT i, i % 300, i % 7, i % 100, 'tag' || i % 11,
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 20 = 0 THEN 'mid' END, CASE WHEN i % 6000 = 0 THEN 'rareterm' END, 'tok' || i % 13, 'base'),
	ARRAY[i % 3, i % 5]
FROM generate_series(1, 40000) i;
CREATE INDEX tm_text ON tm USING sift (body);
ANALYZE tm;
SELECT busy FROM sift.table_maintain('tm') \gset

CREATE FUNCTION node_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAggV6' THEN n := 'SiftAggV6'; END IF;
		IF n = 'SiftAggV6' AND l ~ 'Search: tm_text' THEN n := 'SiftAggV6 text'; END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION text_same(q text, OUT node text, OUT diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'CREATE TEMP TABLE ts_a AS ' || q;
	EXECUTE 'CREATE TEMP TABLE ts_b AS ' || replace(q, ' WHERE ', ' WHERE random() < 2 AND ');
	SELECT count(*) INTO diff FROM ((TABLE ts_a EXCEPT ALL TABLE ts_b) UNION ALL (TABLE ts_b EXCEPT ALL TABLE ts_a)) d;
	DROP TABLE ts_a, ts_b;
	node := node_of(q);
END $$;

CREATE TABLE tm_q (name text PRIMARY KEY, q text);
INSERT INTO tm_q VALUES
	('count',		'SELECT count(*) FROM tm WHERE body @@> ''mid'''),
	('groups',		'SELECT region, count(*), sum(amount) FROM tm WHERE body @@> ''mid'' GROUP BY region'),
	('text_qual',	'SELECT region, count(*), sum(amount) FROM tm WHERE body @@> ''mid'' AND tag <> ''tag1'' GROUP BY region'),
	('like_qual',	'SELECT tag, count(*) FROM tm WHERE body @@> ''common'' AND tag LIKE ''tag1%'' GROUP BY tag'),
	('text_key',	'SELECT tag, count(*), min(amount) FROM tm WHERE body @@> ''tok3'' GROUP BY tag'),
	('distinct',	'SELECT region, count(DISTINCT tenant) FROM tm WHERE body @@> ''tok3'' AND amount > 10 GROUP BY region'),
	('rare',		'SELECT tenant, count(*) FROM tm WHERE body @@> ''rareterm'' GROUP BY tenant'),
	('no_match',	'SELECT region, count(*) FROM tm WHERE body @@> ''nosuchword'' GROUP BY region'),
	('phrase',		'SELECT region, count(*) FROM tm WHERE body @@> ''"common tok3"'' GROUP BY region'),
	('or_terms',	'SELECT region, count(*) FROM tm WHERE body @@> ''rareterm OR mid'' GROUP BY region'),
	('having',		'SELECT region, count(*) FROM tm WHERE body @@> ''mid'' GROUP BY region HAVING count(*) > 100'),
	('topn',		'SELECT tenant, count(*) AS n FROM tm WHERE body @@> ''mid'' AND region < 3 GROUP BY tenant ORDER BY n DESC, tenant LIMIT 5'),
	('rows_topn',	'SELECT id, amount FROM tm WHERE body @@> ''mid'' AND region < 3 ORDER BY amount, id LIMIT 5'),
	('semi_join',	'SELECT region, count(*) FROM tm WHERE body @@> ''mid'' AND tenant IN (SELECT tenant FROM tm WHERE amount = 7) GROUP BY region'),
	('unnest',		'SELECT u, count(*) FROM tm, unnest(tm.arr) u WHERE tm.body @@> ''mid'' GROUP BY u');

\echo -- the answers are those of the statement without the text intake, and the node that read the table
SELECT name, (text_same(q)).* FROM tm_q ORDER BY name;

\echo -- after updates, deletes and inside the transaction that changed them
UPDATE tm SET body = body || ' mid' WHERE id % 53 = 0;
DELETE FROM tm WHERE id % 41 = 0;
SELECT name, (text_same(q)).* FROM tm_q WHERE name IN ('count', 'groups', 'text_qual', 'text_key', 'rows_topn') ORDER BY name;
BEGIN;
UPDATE tm SET body = 'mid gone' WHERE id % 61 = 0;
INSERT INTO tm SELECT 100000 + i, i, i % 7, i, 'tag1', 'mid fresh', ARRAY[i] FROM generate_series(1, 500) i;
SELECT name, (text_same(q)).* FROM tm_q WHERE name IN ('count', 'groups', 'text_qual', 'text_key') ORDER BY name;
ROLLBACK;

\echo -- a prepared statement
PREPARE tm_p(int) AS SELECT region, count(*) FROM tm WHERE body @@> 'mid' AND amount > $1 GROUP BY region ORDER BY region;
EXECUTE tm_p(10);
EXECUTE tm_p(90);

DROP TABLE tm, tm_q;
DROP FUNCTION node_of(text), text_same(text);
DROP EXTENSION pg_sift;
