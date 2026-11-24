-- The rows of a Top-N scan of SiftAggV6 (ORDER BY text keys, LIMIT) read from a part whose text columns are stored as a dictionary: the scan hands its keys to the heap of the best rows as
-- the strings, not as the codes of the dictionary (which the groups of an aggregate fold as they are). Every query is run with the node and without it (sift.enable_agg) and the two
-- answers are compared in order; the first word says they agree, the second that the plan is the node's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE tr (id int PRIMARY KEY, tenant_id int NOT NULL, source text NOT NULL, status text, lang text, memo text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tr SELECT i, i % 400, (ARRAY['web', 'app', 'pos', 'api'])[1 + i % 4], CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['open', 'paid', 'void'])[1 + i % 3] END,
	CASE WHEN i % 7 = 0 THEN '' ELSE (ARRAY['en', 'pt', 'es'])[1 + i % 3] END, CASE WHEN i % 5 = 0 THEN 'express parcel' ELSE 'plain parcel' END
FROM generate_series(1, 4000) i;
CREATE INDEX tr_col ON tr USING sift_col (id, tenant_id, source, status, lang);
CREATE INDEX tr_memo ON tr USING sift (memo);
VACUUM (FREEZE, ANALYZE) tr;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY n)) FROM (SELECT row_number() OVER () AS n, x AS r FROM (' || q || ') x) r' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	a := answer(q);
	SET LOCAL sift.enable_agg = off;
	b := answer(q);
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAgg%' THEN 'node' ELSE 'normal' END) || ' ' || q;
END $$;

SELECT same(q) FROM unnest(ARRAY[
	'SELECT id, source FROM tr ORDER BY source, id LIMIT 20',
	'SELECT id, source FROM tr WHERE tenant_id < 100 ORDER BY source DESC, id LIMIT 20',
	'SELECT id, source, status FROM tr WHERE tenant_id < 300 ORDER BY source, status NULLS FIRST, id LIMIT 30',
	'SELECT id, lang FROM tr WHERE tenant_id > 50 ORDER BY lang DESC, id DESC LIMIT 25 OFFSET 10'
]) q;

-- the hits of a text query are the rows the scan keeps, whatever the keys
SELECT same(q) FROM unnest(ARRAY[
	'SELECT id, tenant_id, source, status, lang FROM tr WHERE memo @@> ''express'' ORDER BY tenant_id DESC, source, status NULLS FIRST, lang DESC, id LIMIT 20',
	'SELECT id, tenant_id FROM tr WHERE memo @@> ''express'' AND tenant_id > 200 ORDER BY tenant_id, id LIMIT 20',
	'SELECT id, source FROM tr WHERE memo @@> ''absent'' ORDER BY source, id LIMIT 20'
]) q;

DELETE FROM tr WHERE id % 9 = 0;
SELECT same('SELECT id, source, status FROM tr WHERE tenant_id < 300 ORDER BY source, status NULLS FIRST, id LIMIT 30');
UPDATE tr SET source = 'web', lang = 'zz' WHERE id % 50 = 0;
SELECT same('SELECT id, source, lang FROM tr ORDER BY source, lang, id LIMIT 40');

DROP TABLE tr;
DROP FUNCTION same(text);
DROP FUNCTION answer(text);
DROP EXTENSION pg_sift;
