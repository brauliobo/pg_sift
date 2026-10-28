-- the elements of an array column (docs/COL-SHRED.md): an index with a path name[] text keeps the elements of a text[] column as the list of a jsonb array, with a
-- point directory that lists the rows that own a value. Every query runs with and without the pushdown and the rows must be the same; the shapes with 'scan'
-- ran the pushdown. The same after every kind of write, and col_verify checks the directory against the chunks each time.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE FUNCTION gen(a int, b int) RETURNS TABLE(id int, tags text[], ids int[]) LANGUAGE sql AS $$
	SELECT i, CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN '{}' WHEN i % 19 = 0 THEN ARRAY['t1', NULL, 'u1', 't1']
	               ELSE ARRAY['t' || i % 500, 'u' || i % 3, 'w' || i % 1000] END,
	       CASE WHEN i % 11 = 0 THEN NULL WHEN i % 23 = 0 THEN '{}' WHEN i % 29 = 0 THEN ARRAY[1, NULL, 7] ELSE ARRAY[i % 400, i % 5 + 1000, i % 7 + 2000] END
	FROM generate_series(a, b) i $$;
CREATE TABLE ae (id int, tags text[], ids int[]) WITH (autovacuum_enabled = false);
INSERT INTO ae SELECT * FROM gen(1, 30000);
CREATE INDEX ae_col ON ae USING sift_col (id, tags, ids) WITH (paths = 'tags[] text, ids[] int4');
VACUUM (FREEZE, ANALYZE) ae;
SELECT sift.col_verify('ae_col', true);
CREATE TABLE ad (LIKE ae) WITH (autovacuum_enabled = false);
INSERT INTO ad SELECT * FROM gen(1, 30000);
-- the plus keeps the image of tags beside its elements: equality and GROUP BY of whole arrays are answered, the element tests by the directory
CREATE INDEX ad_col ON ad USING sift_col (id, tags, ids) WITH (paths = '+tags[] text, ids[] int4');
VACUUM (FREEZE, ANALYZE) ad;
SELECT sift.col_verify('ad_col', true);
SELECT leaf, path, type FROM sift.col_leaves('ad_col') ORDER BY leaf;

CREATE FUNCTION chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; l text; pushed boolean := false; e_on text; e_off text;
BEGIN
	SET sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP IF l ~ 'Custom Scan \(SiftAgg\)' THEN pushed := true; END IF; END LOOP;
	BEGIN
		EXECUTE 'CREATE TEMP TABLE r_on AS ' || q;
	EXCEPTION WHEN OTHERS THEN e_on := SQLERRM; END;
	SET sift.enable_agg = off;
	SET enable_sort = off;
	BEGIN
		EXECUTE 'CREATE TEMP TABLE r_off AS ' || q;
	EXCEPTION WHEN OTHERS THEN e_off := SQLERRM; END;
	RESET sift.enable_agg;
	RESET enable_sort;
	IF e_on IS NOT NULL OR e_off IS NOT NULL THEN
		DROP TABLE IF EXISTS r_on, r_off;
		RETURN CASE WHEN e_on IS NOT DISTINCT FROM e_off THEN 'error: ' || e_on ELSE 'DIFF on=' || coalesce(e_on, 'ok') || ' off=' || coalesce(e_off, 'ok') END;
	END IF;
	SELECT count(*) INTO a FROM ((SELECT d::text FROM r_on d EXCEPT ALL SELECT d::text FROM r_off d)
								  UNION ALL (SELECT d::text FROM r_off d EXCEPT ALL SELECT d::text FROM r_on d)) x;
	DROP TABLE r_on, r_off;
	RETURN (CASE WHEN pushed THEN 'scan ' ELSE 'plain ' END) || CASE WHEN a = 0 THEN 'same' ELSE 'DIFF ' || a END;
END $$;

CREATE FUNCTION shapes(tag text, t text) RETURNS TABLE(shape text, result text) LANGUAGE sql AS $$
	SELECT tag || ' ' || s.name, chk(format(s.q, t)) FROM (VALUES
		('rare', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags @> ARRAY['t7']$q$),
		('common', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags @> ARRAY['u1']$q$),
		('two', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags @> ARRAY['t7', 'u1']$q$),
		('absent', $q$SELECT count(*) AS c FROM %1$I WHERE tags @> ARRAY['zz']$q$),
		('overlap', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags && ARRAY['t7', 'w999', 'zz']$q$),
		('any', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE 'w5' = ANY (tags)$q$),
		('not any', $q$SELECT count(*) AS c FROM %1$I WHERE NOT ('u2' = ANY (tags))$q$),
		('contained', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags <@ ARRAY['t1', 'u1', 't7', 'w7']$q$),
		('not two', $q$SELECT count(*) AS c FROM %1$I WHERE NOT (tags @> ARRAY['t7', 'u1'])$q$),
		('not overlap', $q$SELECT count(*) AS c FROM %1$I WHERE NOT (tags && ARRAY['t7', 'u1'])$q$),
		('not contained', $q$SELECT count(*) AS c FROM %1$I WHERE NOT (tags <@ ARRAY['t1', 'u1', 't7', 'w7'])$q$),
		('null element', $q$SELECT count(*) AS c FROM %1$I WHERE tags @> ARRAY['t1'] AND id %% 19 = 0$q$),
		('group', $q$SELECT tags[2] AS u, count(*) AS c FROM %1$I WHERE tags @> ARRAY['t7'] GROUP BY 1$q$),
		('empty', $q$SELECT count(*) AS c FROM %1$I WHERE tags = '{}'$q$),
		('length', $q$SELECT count(*) AS c FROM %1$I WHERE cardinality(tags) = 2 OR tags IS NULL$q$),
		('whole array', $q$SELECT tags, count(*) AS c FROM %1$I GROUP BY 1$q$),
		('equal', $q$SELECT count(*) AS c FROM %1$I WHERE tags = ARRAY['t7', 'u1', 'w7']$q$),
		('equal two', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags = ARRAY['t7', 'u1']$q$),
		('equal swapped', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags = ARRAY['u1', 't7']$q$),
		('equal repeated', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags = ARRAY['u1', 'u1']$q$),
		('equal 2-D', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags = ARRAY[['t7'], ['u1']]$q$),
		('equal bounds', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE tags = '[0:1]={t7,u1}'$q$),
		('not equal', $q$SELECT count(*) AS c FROM %1$I WHERE NOT (tags = ARRAY['t7', 'u1'])$q$),
		('equal group', $q$SELECT id %% 3 AS g, count(*) AS c FROM %1$I WHERE tags = ARRAY['t7', 'u1'] GROUP BY 1$q$),
		('int equal', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE ids = ARRAY[7, 1001]$q$),
		('int equal swapped', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE ids = ARRAY[1001, 7]$q$),
		('int contains', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE ids @> ARRAY[7]$q$),
		('int two', $q$SELECT count(*) AS c FROM %1$I WHERE ids @> ARRAY[7, 2003]$q$),
		('int overlap', $q$SELECT count(*) AS c, sum(id) AS s FROM %1$I WHERE ids && ARRAY[7, 1001, 99999]$q$),
		('int any', $q$SELECT count(*) AS c FROM %1$I WHERE 2004 = ANY (ids)$q$),
		('int contained', $q$SELECT count(*) AS c FROM %1$I WHERE ids <@ ARRAY[7, 1001, 2003, 1, 2, 1000, 2000, 1002, 2001, 2002, 3, 4]$q$),
		('int not contained', $q$SELECT count(*) AS c FROM %1$I WHERE NOT (ids <@ ARRAY[7, 1001, 2003, 1, 2, 1000, 2000, 1002, 2001, 2002, 3, 4])$q$),
		('int null element', $q$SELECT count(*) AS c FROM %1$I WHERE ids @> ARRAY[1]$q$),
		('keys', $q$SELECT id %% 5 AS k, (tags @> ARRAY['t3']) AS t, count(*) AS c FROM %1$I WHERE tags IS NULL AND id %% 17 = 3 GROUP BY 1, 2$q$),
		('count', $q$SELECT count(tags) AS c, count(DISTINCT tags) AS d FROM %1$I$q$)) s(name, q) $$;

-- the index keeps no image of the arrays: a statement that needs the whole array reads the table, the others the elements
SELECT leaf, path, type FROM sift.col_leaves('ae_col') ORDER BY leaf;
SELECT * FROM shapes('built', 'ae') UNION ALL SELECT * FROM shapes('built', 'ad');
-- the same on arrays that cannot be NULL, where a NOT may be pushed
CREATE TABLE an (id int, tags text[] NOT NULL, ids int[] NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO an SELECT * FROM gen(1, 30000) WHERE tags IS NOT NULL AND ids IS NOT NULL;
CREATE INDEX an_col ON an USING sift_col (id, tags, ids) WITH (paths = 'tags[] text, ids[] int4');
VACUUM (FREEZE, ANALYZE) an;
SELECT * FROM shapes('not null', 'an');
DROP TABLE an;
INSERT INTO ae SELECT * FROM gen(30001, 34000);
INSERT INTO ad SELECT * FROM gen(30001, 34000);
SELECT * FROM shapes('insert', 'ae') UNION ALL SELECT * FROM shapes('insert', 'ad');
UPDATE ae SET tags = ARRAY['t7', 't' || id % 500], ids = ARRAY[7, id % 400] WHERE id % 11 = 0;
UPDATE ad SET tags = ARRAY['t7', 't' || id % 500], ids = ARRAY[7, id % 400] WHERE id % 11 = 0;
SELECT * FROM shapes('update', 'ae') UNION ALL SELECT * FROM shapes('update', 'ad');
-- arrays of two elements in both orders, repeated, of two dimensions and from another bound: the list of a row holds the same elements in all of them
UPDATE ae SET tags = ARRAY['t7', 'u1'], ids = ARRAY[7, 1001] WHERE id % 61 = 0;
UPDATE ae SET tags = ARRAY['u1', 't7'], ids = ARRAY[1001, 7] WHERE id % 67 = 0;
UPDATE ae SET tags = ARRAY['u1', 'u1'] WHERE id % 71 = 0;
UPDATE ae SET tags = ARRAY[['t7'], ['u1']] WHERE id % 73 = 0;
UPDATE ae SET tags = '[0:1]={t7,u1}' WHERE id % 79 = 0;
UPDATE ad SET tags = ARRAY['t7', 'u1'], ids = ARRAY[7, 1001] WHERE id % 61 = 0;
UPDATE ad SET tags = ARRAY['u1', 't7'], ids = ARRAY[1001, 7] WHERE id % 67 = 0;
UPDATE ad SET tags = ARRAY['u1', 'u1'] WHERE id % 71 = 0;
UPDATE ad SET tags = ARRAY[['t7'], ['u1']] WHERE id % 73 = 0;
UPDATE ad SET tags = '[0:1]={t7,u1}' WHERE id % 79 = 0;
SELECT * FROM shapes('orders', 'ae') UNION ALL SELECT * FROM shapes('orders', 'ad');
DELETE FROM ae WHERE id % 7 = 0;
DELETE FROM ad WHERE id % 7 = 0;
SELECT * FROM shapes('delete', 'ae') UNION ALL SELECT * FROM shapes('delete', 'ad');
SELECT sift.col_verify('ae_col', true), sift.col_verify('ad_col', true);
VACUUM ae;
VACUUM ad;
SELECT * FROM shapes('vacuum', 'ae') UNION ALL SELECT * FROM shapes('vacuum', 'ad');
SELECT sift.col_merge('ae_col', true) > 0 AS merged, sift.col_merge('ad_col', true) > 0 AS merged;
SELECT * FROM shapes('merge', 'ae') UNION ALL SELECT * FROM shapes('merge', 'ad');
INSERT INTO ae SELECT * FROM gen(40001, 44000);
INSERT INTO ad SELECT * FROM gen(40001, 44000);
SELECT sift.col_merge('ae_col', true) > 0 AS merged, sift.col_merge('ad_col', true) > 0 AS merged;
SELECT * FROM shapes('merge2', 'ae') UNION ALL SELECT * FROM shapes('merge2', 'ad');
SELECT sift.col_verify('ae_col', true);
REINDEX INDEX ae_col;
REINDEX INDEX ad_col;
SELECT * FROM shapes('reindex', 'ae') UNION ALL SELECT * FROM shapes('reindex', 'ad');

-- a value no row owns is answered by the directory: no unit is read and no heap row vetted
CREATE FUNCTION reads(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF, ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '^ *(Units|Rows):' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM reads($q$SELECT count(*) FROM ae WHERE tags @> ARRAY['zz']$q$);
DROP TABLE ae, ad;
DROP FUNCTION gen(int, int), chk(text), shapes(text, text), reads(text);
DROP EXTENSION pg_sift;
