-- the lists of a jsonb document (paths 'tags[] text, ids[] int4'): a point directory lists the rows that own each value, once however many times the row holds it, and a test on a value reads those rows
-- and not the units. Every query runs with and without the pushdown and the rows must be the same; the shapes with 'scan' ran the pushdown. The same after every kind of write, and col_verify checks the
-- directory against the chunks each time.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE FUNCTION gen(a int, b int) RETURNS TABLE(id int, doc jsonb) LANGUAGE sql AS $$
	SELECT i, jsonb_strip_nulls(jsonb_build_object('tags', CASE WHEN i % 13 = 0 THEN NULL::jsonb WHEN i % 17 = 0 THEN '[]'::jsonb WHEN i % 19 = 0 THEN '["t1", null, "u1", "t1"]'::jsonb
	                                          ELSE jsonb_build_array('t' || i % 500, 'u' || i % 3, 'w' || i % 1000) END,
	                             'ids', CASE WHEN i % 11 = 0 THEN NULL::jsonb WHEN i % 23 = 0 THEN '[]'::jsonb WHEN i % 29 = 0 THEN '[1, null, 7]'::jsonb
	                                         ELSE jsonb_build_array(i % 400, i % 5 + 1000, i % 7 + 2000) END))
	FROM generate_series(a, b) i $$;
CREATE TABLE ld (id int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ld SELECT * FROM gen(1, 30000);
CREATE INDEX ld_col ON ld USING sift_col (id, doc) WITH (paths = 'doc.tags[] text, doc.ids[] int4');
VACUUM (FREEZE, ANALYZE) ld;
SELECT sift.col_verify('ld_col', true);

CREATE FUNCTION chk(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.agg_check_outcome(q) $$;

CREATE FUNCTION shapes(tag text, t text) RETURNS TABLE(shape text, result text) LANGUAGE sql AS $$
	SELECT tag || ' ' || s.name, chk(format(s.q, t)) FROM (VALUES
		('rare', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE doc @> '{"tags": ["t7"]}'$q$),
		('common', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE doc @> '{"tags": ["u1"]}'$q$),
		('two', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE doc @> '{"tags": ["t7", "u1"]}'$q$),
		('absent', $q$SELECT count(*) AS c FROM %1$I WHERE doc @> '{"tags": ["zz"]}'$q$),
		('exists', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE EXISTS (SELECT 1 FROM jsonb_array_elements_text(doc->'tags') e WHERE e = 'w5')$q$),
		('exists in', $q$SELECT count(*) AS c FROM %1$I WHERE EXISTS (SELECT 1 FROM jsonb_array_elements_text(doc->'tags') e WHERE e IN ('t7', 'w999', 'zz'))$q$),
		('not exists', $q$SELECT count(*) AS c FROM %1$I WHERE NOT EXISTS (SELECT 1 FROM jsonb_array_elements_text(doc->'tags') e WHERE e = 'u2')$q$),
		('null element', $q$SELECT count(*) AS c FROM %1$I WHERE doc @> '{"tags": ["t1"]}' AND id %% 19 = 0$q$),
		('group', $q$SELECT doc->'tags'->>1 AS u, count(*) AS c FROM %1$I WHERE doc @> '{"tags": ["t7"]}' GROUP BY 1$q$),
		('int contains', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE doc @> '{"ids": [7]}'$q$),
		('int two', $q$SELECT count(*) AS c FROM %1$I WHERE doc @> '{"ids": [7, 2003]}'$q$),
		('int exists', $q$SELECT count(*) AS c FROM %1$I WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(doc->'ids') e WHERE CASE WHEN jsonb_typeof(e) = 'number' THEN (e)::int END = 2004)$q$),
		('int null element', $q$SELECT count(*) AS c FROM %1$I WHERE doc @> '{"ids": [1]}'$q$)) s(name, q) $$;

SELECT * FROM shapes('built', 'ld');
INSERT INTO ld SELECT * FROM gen(30001, 34000);
SELECT * FROM shapes('insert', 'ld');
UPDATE ld SET doc = jsonb_build_object('tags', jsonb_build_array('t7', 't' || id % 500), 'ids', jsonb_build_array(7, id % 400)) WHERE id % 11 = 0;
SELECT * FROM shapes('update', 'ld');
DELETE FROM ld WHERE id % 7 = 0;
SELECT * FROM shapes('delete', 'ld');
SELECT sift.col_verify('ld_col', true);
VACUUM ld;
SELECT * FROM shapes('vacuum', 'ld');
SELECT sift.col_merge('ld_col', true) > 0 AS merged;
SELECT * FROM shapes('merge', 'ld');
INSERT INTO ld SELECT * FROM gen(40001, 44000);
SELECT sift.col_merge('ld_col', true) > 0 AS merged;
SELECT * FROM shapes('merge2', 'ld');
SELECT sift.col_verify('ld_col', true);
REINDEX INDEX ld_col;
SELECT * FROM shapes('reindex', 'ld');

-- a value no row owns is answered by the directory: no unit is read and no heap row vetted
CREATE FUNCTION reads(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF, ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^ *(Units|Rows):' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM reads($q$SELECT count(*) FROM ld WHERE doc @> '{"tags": ["zz"]}'$q$);
DROP TABLE ld;
DROP FUNCTION gen(int, int), chk(text), shapes(text, text), reads(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
