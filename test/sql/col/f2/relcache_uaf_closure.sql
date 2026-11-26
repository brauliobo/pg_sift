-- The cached state of a sift_col index (rd_amcache) is freed whenever its relcache entry is reloaded, which a lock or a catalog lookup does; the code that keeps a pointer to it across such a point
-- reads freed memory. sift.test_col_rel_inval overwrites the state and invalidates the entry where the first touch of the index and the build of a private copy look up the catalogs, so
-- that every reader of Format 2 that holds the state across one of them dies or reads garbage here: the order the planner offers, the Top-N and the grouped aggregate over the index, the unnest
-- of a column of the table and of a jsonb array read through an index expression, the map of a document, and the refresh of a Format 1 rollup. Every query runs over the index and over a heap
-- twin (the truth): the answer must be the twin's and the backend must live.
-- requires: build col
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.agg_force = on;
SET sift.test_create_format1 = off;
SET jit = off;

CREATE FUNCTION pe_norm(d jsonb) RETURNS jsonb LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE AS $$ BEGIN RETURN d; END $$;

CREATE FUNCTION same(q text, ix text, hp text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', ix)) INTO a;
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', hp)) INTO b;
	RETURN CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;

CREATE TABLE rh (id int, k int NOT NULL, g int, v int, tag text, items jsonb, doc jsonb, arr int[]);
INSERT INTO rh SELECT i, i, i % 7, i % 13, 'tag' || i % 5,
	CASE WHEN i % 12 = 0 THEN NULL ELSE jsonb_build_array(jsonb_build_object('kind', 'k' || i % 3, 'qty', i % 9), jsonb_build_object('kind', 'k' || i % 2, 'qty', 1)) END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE jsonb_build_object('lang', 'en', 'a', i % 10, 'b', i % 7, 'parts', jsonb_build_array(
		jsonb_build_object('category', (ARRAY['Toys', 'Books', 'Food', '5'])[i % 4 + 1], 'val', (ARRAY['a', 'b', 'true'])[i % 3 + 1], 'score', i % 10 / 2.0),
		jsonb_build_object('category', (ARRAY['Food', 'Toys', '5', 'Books'])[i % 4 + 1], 'val', (ARRAY['b', 'true', 'a'])[i % 3 + 1], 'score', i % 7 / 2.0))) END,
	ARRAY[i % 5, 1]
FROM generate_series(1, 600) i;
ALTER TABLE rh ADD PRIMARY KEY (id);
CREATE TABLE rc (LIKE rh INCLUDING INDEXES);
INSERT INTO rc SELECT * FROM rh;
CREATE INDEX rc_col ON rc USING sift_col (k, g, v, tag, items, pe_norm(doc), arr)
	WITH (key_order = 'k', paths = 'items[]{kind text, qty int4}, arr[] int4, pe_norm.a float8, pe_norm.b float8, pe_norm.parts[]{category astext, val astext, score float8}');
VACUUM (FREEZE, ANALYZE) rh, rc;
SELECT sift._test_index_format('rc_col') AS format_major;

SET sift.test_col_rel_inval = on;

-- the order of the key columns, the Top-N and the grouped aggregate
SELECT same(q, 'rc', 'rh') FROM unnest(ARRAY[
	'SELECT k FROM @T ORDER BY k LIMIT 5',
	'SELECT k, v FROM @T ORDER BY k DESC LIMIT 5',
	'SELECT g, count(*), sum(v), max(tag) FROM @T GROUP BY g',
	'SELECT g, v, count(*) FROM @T WHERE k > 100 GROUP BY g, v'
]) q;

-- the unnest of a jsonb array and of an array column beside columns of the table (the first touch of the leaves of the index is in the scan)
SELECT same(q, 'rc', 'rh') FROM unnest(ARRAY[
	'SELECT i.kind, count(*), sum(i.qty) FROM @T o, jsonb_to_recordset(o.items) AS i(kind text, qty int) GROUP BY 1',
	'SELECT o.g, o.v, i.kind, count(*) FROM @T o, jsonb_to_recordset(o.items) AS i(kind text, qty int) GROUP BY 1, 2, 3',
	'SELECT o.tag, o.g, o.v % 4, sum(a) FROM @T o, unnest(o.arr) a GROUP BY 1, 2, 3'
]) q;

-- elements of a key of the document an index expression holds, with several PostgreSQL expressions of the elements: each prepares an expression, which reads the catalogs
SELECT same(q, 'rc', 'rh') FROM unnest(ARRAY[
	'SELECT p.category, count(*), sum(p.score), sum(CASE WHEN p.val = ''a'' THEN p.score END) FROM @T o, jsonb_to_recordset(pe_norm(o.doc) -> ''parts'') AS p(category text, val text, score float8) GROUP BY 1',
	'SELECT o.g, p.val, lower(p.category), count(*), avg(p.score * 2) FROM @T o CROSS JOIN LATERAL jsonb_to_recordset(pe_norm(o.doc) -> ''parts'') AS p(category text, val text, score float8) GROUP BY 1, 2, 3',
	'SELECT o.g, count(*) FROM @T o LEFT JOIN LATERAL jsonb_to_recordset(pe_norm(o.doc) -> ''parts'') AS p(category text, val text) ON true GROUP BY 1'
]) q;

-- the map of a document
SELECT same(q, 'rc', 'rh') FROM unnest(ARRAY[
	'SELECT e.key, count(*), sum(e.value::float8) FROM @T o, jsonb_each_text(o.doc) e WHERE e.key IN (''a'', ''b'') GROUP BY 1'
]) q;
RESET sift.test_col_rel_inval;

-- the refresh of the rollup of a Format 1 index takes the maintenance lock, which accepts the invalidations that reload the entry
SET sift.test_create_format1 = on;
CREATE TABLE fh (g int, v int, n numeric(12,2), tag text);
INSERT INTO fh SELECT i % 7, i % 13, (i % 997) / 8.0, 'tag' || i % 5 FROM generate_series(1, 3000) i;
CREATE INDEX f1_col ON fh USING sift_col (g, v, n, tag);
ANALYZE fh;
SELECT sift._test_index_format('f1_col') AS format_major;
SET sift.test_col_rel_inval = on;
SELECT sift.col_refresh('f1_col') IS NOT NULL AS refreshed;
SELECT count(*), sum(v) FROM fh WHERE g = 3;
RESET sift.test_col_rel_inval;

DROP TABLE rc, rh, fh;
DROP FUNCTION pe_norm(jsonb), same(text, text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
