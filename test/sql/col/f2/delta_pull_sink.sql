-- What the unnest scan of Format 2 reads through the pull of the index (col/f2/sink/ext.c) beside the column deltas of the snapshot: a map of a document whose entries a delta moved (the sums of
-- the granule are not the answer of a granule a delta changes), a JSON null a delta wrote, the Top-N of a column a delta moved or emptied (the zones of the granule say nothing of the patched
-- column), and a char(n) column read beside the elements (its chunk is a dictionary of codes, decoded as the strings they stand for). Every query runs over the index and over a heap twin
-- the same statements ran on (no index): the answer must be the twin's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.agg_force = on;
SET jit = off;
SELECT current_setting('sift.test_create_format1', true) IS NOT NULL AS testing \gset
\if :testing
SET sift.test_create_format1 = off;
\endif

CREATE FUNCTION same(q text, ix text, hp text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', ix) LOOP plan := plan || l; END LOOP;
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', ix)) INTO a;
	EXECUTE format('SELECT coalesce(md5(string_agg(r::text, ''|'' ORDER BY r::text)), '''') FROM (%s) r', replace(q, '@T', hp)) INTO b;
	RETURN (CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6%' THEN 'node' ELSE 'normal' END);
END $$;

CREATE TABLE dm (id int NOT NULL, doc jsonb);
INSERT INTO dm SELECT i, jsonb_build_object('a', i % 10, 'b', i % 7, 'c', i % 3) FROM generate_series(1, 4000) i;
CREATE TABLE dmh (LIKE dm);
INSERT INTO dmh SELECT * FROM dm;
CREATE INDEX dm_c ON dm USING sift_col (id, doc) WITH (key_order = 'id', paths = 'doc.* float8');

CREATE TABLE zt (id int NOT NULL, v int, pad text);
INSERT INTO zt SELECT i, i % 1000, 'p' || i FROM generate_series(1, 8000) i;
CREATE TABLE zth (LIKE zt);
INSERT INTO zth SELECT * FROM zt;
CREATE INDEX zt_c ON zt USING sift_col (id, v, pad) WITH (key_order = 'id');

CREATE TABLE db (id int NOT NULL, code char(4), items jsonb);
INSERT INTO db SELECT i, 'c' || i % 5, jsonb_build_array(jsonb_build_object('q', i % 5), jsonb_build_object('q', 1)) FROM generate_series(1, 4000) i;
CREATE TABLE dbh (LIKE db);
INSERT INTO dbh SELECT * FROM db;
CREATE INDEX db_c ON db USING sift_col (id, code, items) WITH (key_order = 'id', paths = 'items[]{q int4}');

VACUUM (FREEZE, ANALYZE) dm, dmh, zt, zth, db, dbh;

UPDATE dm SET doc = jsonb_set(doc, '{a}', '1000') WHERE id % 500 = 0;
UPDATE dmh SET doc = jsonb_set(doc, '{a}', '1000') WHERE id % 500 = 0;
UPDATE dm SET doc = jsonb_set(doc, '{b}', 'null') WHERE id % 700 = 0;
UPDATE dmh SET doc = jsonb_set(doc, '{b}', 'null') WHERE id % 700 = 0;
UPDATE zt SET v = 100000 + id WHERE id % 1500 = 0;
UPDATE zth SET v = 100000 + id WHERE id % 1500 = 0;
UPDATE zt SET v = NULL WHERE id % 1999 = 0;
UPDATE zth SET v = NULL WHERE id % 1999 = 0;
SELECT dirty_rows > 0 AS dm_patched FROM sift.status('dm_c');

SELECT same(q, 'dm', 'dmh') FROM unnest(ARRAY[
	'SELECT e.key, count(*), count(e.value), sum(e.value::float8) FROM @T o, jsonb_each_text(o.doc) e GROUP BY 1',
	'SELECT e.key, sum(e.value::float8) FROM @T o, jsonb_each_text(o.doc) e WHERE o.id > 1000 GROUP BY 1'
]) q;

SELECT same(q, 'zt', 'zth') FROM unnest(ARRAY[
	'SELECT id, v FROM @T ORDER BY v DESC NULLS LAST, id LIMIT 8',
	'SELECT id, v FROM @T ORDER BY v NULLS FIRST, id LIMIT 6',
	'SELECT count(*), sum(v), min(v), max(v) FROM @T WHERE v > 500'
]) q;

SELECT same(q, 'db', 'dbh') FROM unnest(ARRAY[
	'SELECT o.code, count(*), sum(i.q) FROM @T o, jsonb_to_recordset(o.items) AS i(q int) GROUP BY 1',
	'SELECT o.code, i.q, count(*) FROM @T o, jsonb_to_recordset(o.items) AS i(q int) WHERE o.code <> ''c3'' GROUP BY 1, 2'
]) q;

DROP TABLE dm, dmh, zt, zth, db, dbh;
DROP FUNCTION same(text, text, text);
DROP EXTENSION pg_sift;
