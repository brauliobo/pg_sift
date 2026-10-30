-- ORDER BY <scanned column> LIMIT n over an inner join to a small table that only filters: the keys of the table's rows that pass its conditions
-- are a set of the scan (`key IN (keys)`), which the top-N holds exactly, so the join has nothing left to test. That needs the table to have a
-- unique index on its join column (no row of the scan matches twice), one integer join column, conditions that read the table's columns alone,
-- and no column of the table in the result. Everything else keeps the normal plan. A bounded differential fuzz (fixed seed, queries made from
-- hashes) compares the pushed top-N with the plain plan (sift.enable_agg off) in order, and the refused shapes are checked one by one.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

-- pad makes a page hold about 5 rows: 30000 rows are about 24 superblocks; g: 0 to 49 and NULLs, the table lacks 40 to 49
CREATE TABLE jf (id serial PRIMARY KEY, g int, h bigint, lab text, k_rand int, k_mix int, pad text DEFAULT repeat('x', 1500));
ALTER TABLE jf ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO jf (g, h, lab, k_rand, k_mix)
SELECT CASE WHEN i % 41 = 0 THEN NULL ELSE (hashint4(i + 7) & 2147483647) % 50 END, (hashint4(i + 8) & 2147483647) % 30,
	   'label_' || (hashint4(i + 9) & 2147483647) % 40, (hashint4(i + 1) & 2147483647) % 1000000,
	   CASE WHEN i % 9 = 0 THEN NULL ELSE (hashint4(i + 2) & 2147483647) % 50 END
FROM generate_series(1, 30000) i;
CREATE INDEX jf_col ON jf USING sift_col (id, g, h, lab, k_rand, k_mix) WITH (write_buffer_limit = 256, point_tid_columns = 'off');

CREATE TABLE jd (g int PRIMARY KEY, label text, region int);
INSERT INTO jd SELECT x, 'label_' || x, x % 5 FROM generate_series(0, 39) x;
CREATE TABLE jd2 (h bigint PRIMARY KEY, tier int);
INSERT INTO jd2 SELECT x, x % 3 FROM generate_series(0, 24) x;
CREATE TABLE jn (g int, label text);			-- no unique index: a row of the scan may match more than once
INSERT INTO jn SELECT x % 40, 'label_' || x % 40 FROM generate_series(1, 80) x;
CREATE TABLE jl (label text PRIMARY KEY, region int);	-- a text key
INSERT INTO jl SELECT 'label_' || x, x % 5 FROM generate_series(0, 39) x;
VACUUM (ANALYZE) jf;
VACUUM (ANALYZE) jd;
VACUUM (ANALYZE) jd2;
VACUUM (ANALYZE) jn;
VACUUM (ANALYZE) jl;

CREATE FUNCTION jt_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_nestloop', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

CREATE FUNCTION jt_is_topn(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text; topn bool := false;
BEGIN
	PERFORM jt_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP topn := topn OR line ~ 'Top-N: '; END LOOP;
	RETURN topn;
END $$;

-- one query in both plans: the md5 of its rows in order, and whether the pushed plan is the top-N scan
CREATE FUNCTION jt_run(q text, OUT pushed_md5 text, OUT plain_md5 text, OUT topn bool) LANGUAGE plpgsql AS $$
BEGIN
	topn := jt_is_topn(q);
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO pushed_md5;
	PERFORM jt_mode(false);
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO plain_md5;
END $$;

-- the query number i of a stage, from hashes of (seed, i)
CREATE FUNCTION jt_query(seed int, i int) RETURNS text LANGUAGE plpgsql IMMUTABLE AS $$
DECLARE
	h int[] := ARRAY(SELECT hashint4(seed * 100003 + i * 31 + x) & 2147483647 FROM generate_series(1, 10) x);
	key text := (ARRAY['f.k_rand', 'f.k_mix', 'f.id'])[1 + h[1] % 3];
	ord text := key || CASE h[2] % 2 WHEN 0 THEN '' ELSE ' DESC' END || (ARRAY['', ' NULLS FIRST', ' NULLS LAST'])[1 + h[3] % 3];
	lim int := (ARRAY[1, 3, 10, 100, 700])[1 + h[4] % 5];
	off int := (ARRAY[0, 0, 0, 7, 50])[1 + h[5] % 5];
	frm text := CASE h[6] % 8
		WHEN 0 THEN 'jf f JOIN jd d ON d.g = f.g WHERE d.label = ''label_' || h[7] % 45 || ''''
		WHEN 1 THEN 'jf f JOIN jd d ON d.g = f.g WHERE d.region IN (' || h[7] % 5 || ', ' || (h[7] + 2) % 5 || ')'
		WHEN 2 THEN 'jf f JOIN jd d ON d.g = f.g AND d.region = ' || h[7] % 5 || ' WHERE f.k_mix > ' || h[8] % 50
		WHEN 3 THEN 'jf f JOIN jd d ON d.g = f.g'
		WHEN 4 THEN 'jf f JOIN jd d ON d.g = f.g JOIN jd2 e ON e.h = f.h WHERE d.region = ' || h[7] % 5 || ' AND e.tier = ' || h[8] % 3
		WHEN 5 THEN 'jf f JOIN jd d ON f.g = d.g WHERE d.label < ''label_' || h[7] % 40 || ''' AND f.k_rand < ' || h[8] % 1000000
		WHEN 6 THEN 'jd d JOIN jf f ON d.g = f.g WHERE d.region <> ' || h[7] % 5
		ELSE 'jf f JOIN jd2 e ON e.h = f.h WHERE e.tier = ' || h[7] % 3 END;
BEGIN
	RETURN CASE h[9] % 3
		WHEN 0 THEN format('SELECT f.id, %s FROM %s ORDER BY %s, f.id LIMIT %s OFFSET %s', key, frm, ord, lim, off)
		WHEN 1 THEN format('SELECT f.id, %s FROM %s ORDER BY %s, f.id DESC LIMIT %s OFFSET %s', key, frm, ord, lim, off)
		ELSE format('SELECT %s FROM %s ORDER BY %s LIMIT %s OFFSET %s', key, frm, ord, lim, off) END;
END $$;

-- N queries of a stage; the first differences are listed, so that a failure names its queries
CREATE FUNCTION jt_fuzz(label text, seed int, n int) RETURNS TABLE (stage text, queries int, different int, mostly_topn bool, first_different text)
LANGUAGE plpgsql AS $$
DECLARE r record; q text; bad int := 0; pushed int := 0; first text[] := '{}';
BEGIN
	FOR i IN 1..n LOOP
		q := jt_query(seed, i);
		SELECT * INTO r FROM jt_run(q);
		IF r.topn THEN pushed := pushed + 1; END IF;
		IF r.pushed_md5 IS DISTINCT FROM r.plain_md5 THEN
			bad := bad + 1;
			IF bad <= 3 THEN first := first || q; END IF;
		END IF;
	END LOOP;
	RETURN QUERY SELECT label, n, bad, pushed * 2 >= n, array_to_string(first, E'\n');
END $$;

-- the shapes the top-N takes, and the ones that keep the normal plan (the results of both are compared below)
CREATE TEMP TABLE jt_shapes (n int, q text);
INSERT INTO jt_shapes VALUES
	(1, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g WHERE d.label = ''label_7'' ORDER BY f.k_rand, f.id LIMIT 10'),
	(2, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g WHERE d.region = 2 ORDER BY f.k_rand DESC, f.id LIMIT 10'),
	(3, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g AND d.region IN (1, 3) ORDER BY f.k_rand, f.id LIMIT 10'),
	(4, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g JOIN jd2 e ON e.h = f.h WHERE d.region = 1 AND e.tier = 2 ORDER BY f.k_rand, f.id LIMIT 10'),
	(5, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g WHERE f.k_mix > 5 AND d.label = ''label_7'' ORDER BY f.k_rand, f.id LIMIT 10'),
	(6, 'SELECT f.id, f.k_rand FROM jd d JOIN jf f ON f.g = d.g WHERE d.label = ''label_7'' ORDER BY f.k_rand, f.id LIMIT 10'),
	(7, 'SELECT f.id, f.k_rand FROM jf f JOIN jd2 e ON e.h = f.h WHERE e.tier = 1 ORDER BY f.k_rand, f.id LIMIT 10'),
	-- refused: a column of the table in the result, a table with no unique index on the key, an outer join (to a table without a unique index: PostgreSQL 19 removes one to a unique key that nothing reads), a condition that reads the scanned row,
	-- a text key, an expression of the scanned column as the key, a second key, a join to a sub-select, an aggregate
	(11, 'SELECT f.id, d.label FROM jf f JOIN jd d ON d.g = f.g WHERE d.region = 2 ORDER BY f.k_rand, f.id LIMIT 10'),
	(12, 'SELECT f.id, f.k_rand FROM jf f JOIN jn d ON d.g = f.g WHERE d.label = ''label_7'' ORDER BY f.k_rand, f.id LIMIT 10'),
	(13, 'SELECT f.id, f.k_rand FROM jf f LEFT JOIN jn d ON d.g = f.g AND d.label = ''label_7'' ORDER BY f.k_rand, f.id LIMIT 10'),
	(14, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g AND d.region > f.k_mix % 5 ORDER BY f.k_rand, f.id LIMIT 10'),
	(15, 'SELECT f.id, f.k_rand FROM jf f JOIN jl d ON d.label = f.lab WHERE d.region = 2 ORDER BY f.k_rand, f.id LIMIT 10'),
	(16, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g + 1 WHERE d.region = 2 ORDER BY f.k_rand, f.id LIMIT 10'),
	(17, 'SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g AND d.region = f.k_mix % 5 ORDER BY f.k_rand, f.id LIMIT 10'),
	(18, 'SELECT f.id, f.k_rand FROM jf f JOIN (SELECT g FROM jd WHERE region = 2) d ON d.g = f.g ORDER BY f.k_rand, f.id LIMIT 10'),
	(19, 'SELECT f.id, d.region FROM jf f JOIN jd d ON d.g = f.g ORDER BY d.region, f.id LIMIT 10'),
	-- pushed: a sub-select PostgreSQL pulls up into a semi join (IN, EXISTS) is a filter of the same kind; it needs no unique key
	(20, 'SELECT f.id, f.k_rand FROM jf f WHERE f.g IN (SELECT g FROM jd WHERE region = 2) ORDER BY f.k_rand, f.id LIMIT 10');
SELECT s.n, r.topn, r.pushed_md5 = r.plain_md5 AS same_rows FROM jt_shapes s, LATERAL jt_run(s.q) r ORDER BY s.n;

-- the plan: one scan of the fact table, the table's rows loaded as a set (EXPLAIN shows the conjunct), no workers
SELECT jt_mode(true);
SELECT jt_is_topn('SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g WHERE d.region = 2 ORDER BY f.k_rand, f.id LIMIT 10') AS topn_with_join;

SELECT * FROM jt_fuzz('build', 41, 120);

-- the table changes between two runs: the set is read when the scan starts (a deleted row of the table, a changed condition, a new row)
DELETE FROM jd WHERE g IN (3, 8, 13);
UPDATE jd SET region = 4 WHERE g IN (7, 17);
INSERT INTO jd VALUES (40, 'label_40', 2), (41, 'label_41', 4);
SELECT * FROM jt_fuzz('table changed', 42, 120);

-- ids 8000 to 8300 are deleted and not vacuumed (dirty units), then vacuumed; the new versions of updated rows move to other pages with another key
DELETE FROM jf WHERE id BETWEEN 8000 AND 8300;
SELECT * FROM jt_fuzz('dirty', 43, 120);
VACUUM jf;
UPDATE jf SET g = 7, h = 3, k_rand = -5, k_mix = -5 WHERE id IN (5, 6000, 21000);
UPDATE jf SET g = 99, k_rand = -9, k_mix = -9 WHERE id IN (7, 15000);
UPDATE jf SET g = NULL, k_rand = NULL, k_mix = NULL WHERE id IN (9, 22000);
UPDATE jf SET g = 41, k_rand = k_rand / 3 WHERE id BETWEEN 12000 AND 12040;
SELECT * FROM jt_fuzz('moved', 44, 120);

-- rows in the write buffer, and the buffer promoted into units
INSERT INTO jf (g, h, lab, k_rand, k_mix)
SELECT i % 50, i % 30, 'label_' || i % 40, (i * 91) % 1000000, i % 50 FROM generate_series(1, 150) i;
INSERT INTO jf (g, h, lab, k_rand, k_mix) VALUES (7, 3, 'label_7', -1000, -1000), (7, 3, 'label_7', 2000000, 2000000), (7, 3, 'label_7', NULL, NULL), (NULL, 3, 'label_7', -2000, -2000);
SELECT * FROM jt_fuzz('write buffer', 45, 120);
VACUUM jf;
SELECT sift.col_promote('jf_col') >= 0 AS promoted;
SELECT * FROM jt_fuzz('promoted', 46, 120);

-- a generic plan: the condition of the table is a parameter
BEGIN;
SET LOCAL plan_cache_mode = force_generic_plan;
SELECT jt_mode(true);
PREPARE jt_gen (text, bigint) AS SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g WHERE d.label = $1 ORDER BY f.k_rand, f.id LIMIT $2;
SELECT jt_is_topn('EXECUTE jt_gen(''label_7'', 5)') AS generic_is_topn;
CREATE TEMP TABLE jt_new AS EXECUTE jt_gen('label_7', 5);
CREATE TEMP TABLE jt_new2 AS EXECUTE jt_gen('label_nothing', 5);
SELECT jt_mode(false);
CREATE TEMP TABLE jt_ref AS SELECT f.id, f.k_rand FROM jf f JOIN jd d ON d.g = f.g WHERE d.label = 'label_7' ORDER BY f.k_rand, f.id LIMIT 5;
SELECT (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM jt_ref) = (SELECT md5(string_agg(id || ':' || k_rand, ',')) FROM jt_new) AS generic_same,
	   (SELECT count(*) FROM jt_new2) AS generic_none;
DEALLOCATE jt_gen;
COMMIT;

DROP TABLE jf, jd, jd2, jn, jl;
DROP FUNCTION jt_mode(bool), jt_is_topn(text), jt_run(text), jt_query(int, int), jt_fuzz(text, int, int);
DROP EXTENSION pg_sift;
