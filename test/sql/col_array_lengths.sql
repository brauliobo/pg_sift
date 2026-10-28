-- the lengths of an array column: cardinality(x), array_length(x, 1) and x IS [NOT] NULL of an array that an index keeps
-- as two integer lanes are read without the arrays. Every query runs with and without the pushdown and the rows must be the same; the shapes with
-- 'scan' read the lanes. The index with array_lengths = off is the index of a release before the option: the same answers from the images.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE TABLE al (id int, grp int, ia int[], ta text[], na numeric[], ma int[], lb int[]) WITH (autovacuum_enabled = false);
INSERT INTO al SELECT i, i % 7,
	CASE WHEN i % 10 = 0 THEN NULL WHEN i % 14 = 0 THEN '{}' WHEN i % 17 = 0 THEN ARRAY[NULL, NULL]::int[] ELSE ARRAY[i % 5, i % 3, i % 11] END,
	CASE WHEN i % 9 = 0 THEN NULL WHEN i % 13 = 0 THEN '{}' ELSE (SELECT array_agg('t' || (i + k) % 6) FROM generate_series(1, i % 5 + 1) k) END,
	CASE WHEN i % 8 = 0 THEN NULL ELSE ARRAY[(i % 3)::numeric, 1.50] END,
	-- two and three dimensions: array_length(x, 1) is the first, cardinality the product; an empty one has no dimension
	CASE WHEN i % 12 = 0 THEN NULL WHEN i % 6 = 0 THEN ARRAY[[1, 2, 3], [4, 5, 6]] WHEN i % 5 = 0 THEN ARRAY[[[1], [2]], [[3], [4]], [[5], [6]]] WHEN i % 4 = 0 THEN '{}' ELSE ARRAY[1] END,
	-- a lower bound other than 1 changes no length
	CASE WHEN i % 7 = 0 THEN NULL WHEN i % 3 = 0 THEN '[0:2]={1,2,3}'::int[] WHEN i % 11 = 0 THEN (SELECT array_agg(k) FROM generate_series(1, 3000) k) ELSE '[5:5]={9}'::int[] END
FROM generate_series(1, 5000) i;
CREATE INDEX al_col ON al USING sift_col (id, grp, ia, ta, na, ma, lb);
CREATE INDEX al_old ON al USING sift_col (id, grp, ia, ta, na, ma, lb) WITH (array_lengths = off);
VACUUM (FREEZE, ANALYZE) al;
SELECT sift.col_verify('al_col', true), sift.col_verify('al_old', true);
SELECT indexrelid::regclass AS idx, (SELECT option_value FROM pg_options_to_table(reloptions) WHERE option_name = 'array_lengths') AS array_lengths
FROM pg_index JOIN pg_class ON pg_class.oid = indexrelid WHERE indrelid = 'al'::regclass AND indexrelid::regclass::text LIKE 'al\_%' ORDER BY 1;
SELECT leaf, path, type FROM sift.col_leaves('al_col') ORDER BY leaf;
SELECT count(*) AS leaves_without_lengths FROM sift.col_leaves('al_old');
DROP INDEX al_old;

-- the rows of both plans as the text PostgreSQL prints them; scan: the pushdown ran
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

-- the shapes, on the table's one index at a time
CREATE FUNCTION shapes(tag text) RETURNS TABLE(shape text, result text) LANGUAGE sql AS $$
	VALUES (tag || ' presence', chk($q$SELECT count(*) AS c FROM al WHERE ia IS NOT NULL AND cardinality(ia) > 0$q$)),
	       (tag || ' empty', chk($q$SELECT count(*) AS c FROM al WHERE cardinality(ta) = 0$q$)),
	       (tag || ' null', chk($q$SELECT count(*) AS c FROM al WHERE ma IS NULL$q$)),
	       (tag || ' notnull_or', chk($q$SELECT count(*) AS c FROM al WHERE lb IS NOT NULL OR ia IS NULL$q$)),
	       (tag || ' null_filter', chk($q$SELECT grp AS g, count(*) FILTER (WHERE ta IS NULL) AS a, count(*) FILTER (WHERE ta IS NOT NULL AND cardinality(ta) > 2) AS b FROM al GROUP BY grp$q$)),
	       (tag || ' array_length', chk($q$SELECT count(*) AS c FROM al WHERE array_length(ma, 1) >= 2$q$)),
	       (tag || ' array_length_null', chk($q$SELECT count(*) AS c FROM al WHERE array_length(ia, 1) IS NULL$q$)),
	       (tag || ' array_length_dim2', chk($q$SELECT count(*) AS c FROM al WHERE array_length(ma, 2) = 3$q$)),
	       (tag || ' array_length_dim0', chk($q$SELECT count(*) AS c FROM al WHERE array_length(ma, 0) IS NULL$q$)),
	       (tag || ' multidim', chk($q$SELECT cardinality(ma) AS n, array_length(ma, 1) AS d, count(*) AS c FROM al GROUP BY 1, 2$q$)),
	       (tag || ' lower_bound', chk($q$SELECT cardinality(lb) AS n, array_length(lb, 1) AS d, count(*) AS c FROM al GROUP BY 1, 2$q$)),
	       (tag || ' key', chk($q$SELECT cardinality(ia) AS n, count(*) AS c FROM al GROUP BY 1$q$)),
	       (tag || ' aggregates', chk($q$SELECT grp AS g, min(cardinality(ta)) AS a, max(cardinality(ta)) AS b, sum(cardinality(na)) AS s, avg(array_length(ma, 1)) AS v FROM al GROUP BY grp$q$)),
	       (tag || ' distinct', chk($q$SELECT grp AS g, count(DISTINCT id) FILTER (WHERE ia IS NOT NULL AND cardinality(ia) > 0) AS a, count(DISTINCT id) FILTER (WHERE ta IS NOT NULL AND cardinality(ta) > 0) AS b FROM al GROUP BY grp$q$)),
	       (tag || ' expression', chk($q$SELECT count(*) AS c FROM al WHERE coalesce(cardinality(ta), 0) + 1 > 3 AND NOT (cardinality(ia) = 1)$q$)),
	       (tag || ' case', chk($q$SELECT CASE WHEN cardinality(na) IS NULL THEN 'none' WHEN cardinality(na) > 1 THEN 'many' ELSE 'one' END AS k, count(*) AS c FROM al GROUP BY 1$q$)),
	       (tag || ' two_arrays', chk($q$SELECT count(*) AS c FROM al WHERE cardinality(ia) = cardinality(ta)$q$)),
	       (tag || ' with_ops', chk($q$SELECT count(*) AS c FROM al WHERE cardinality(ta) > 1 AND ta @> ARRAY['t2'] AND ia IS NOT NULL$q$)),
	       (tag || ' unnest', chk($q$SELECT e AS k, count(*) AS c FROM al, unnest(ta) e WHERE cardinality(ta) > 2 GROUP BY e$q$)),
	       (tag || ' other_dims', chk($q$SELECT grp AS g, count(*) AS c FROM al WHERE id < 3000 AND cardinality(ia) > 1 GROUP BY grp$q$))
$$;
SELECT * FROM shapes('lanes');
-- the lanes are what the scan reads: the filter names cardinality, not the array
EXPLAIN (COSTS OFF) SELECT count(*) FROM al WHERE ia IS NOT NULL AND cardinality(ia) > 0;
EXPLAIN (COSTS OFF) SELECT grp, avg(cardinality(ta)), max(array_length(ma, 1)) FROM al GROUP BY grp;

-- a generic plan reads the dimension and the bound from parameters
PREPARE g(int, int) AS SELECT count(*) FROM al WHERE array_length(ma, $1) > $2;
SET plan_cache_mode = force_generic_plan;
EXECUTE g(1, 1);
EXECUTE g(2, 1);
RESET plan_cache_mode;
DEALLOCATE g;

-- the write buffer, an update to another shape, a delete, a promotion and a merge
INSERT INTO al SELECT 5000 + i, i % 7, ARRAY[i], CASE WHEN i % 3 = 0 THEN NULL ELSE '{}'::text[] END, ARRAY[i::numeric], ARRAY[[i, i]], '{1,2}' FROM generate_series(1, 300) i;
UPDATE al SET ia = NULL, ta = ARRAY['z'], ma = '{}' WHERE id % 13 = 1;
UPDATE al SET ia = ARRAY[1, 2, 3, 4] WHERE id % 13 = 2;
DELETE FROM al WHERE id % 11 = 3;
SELECT * FROM shapes('buffer');
SELECT sift.col_promote('al_col');
VACUUM al;
SELECT * FROM shapes('promoted');
SELECT sift.col_merge('al_col', true);
SELECT sift.col_verify('al_col', true);
SELECT * FROM shapes('merged');

-- a table without NULLs or empty arrays (a constant chunk), parallel workers, REINDEX, and the option changed
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT * FROM shapes('workers');
RESET max_parallel_workers_per_gather;
REINDEX INDEX al_col;
SELECT sift.col_verify('al_col', true);
SELECT * FROM shapes('reindexed');
ALTER INDEX al_col SET (array_lengths = off);
REINDEX INDEX al_col;
SELECT count(*) AS leaves_after_off FROM sift.col_leaves('al_col');
SELECT sift.col_verify('al_col', true);
EXPLAIN (COSTS OFF) SELECT count(*) FROM al WHERE ia IS NOT NULL AND cardinality(ia) > 0;
SELECT * FROM shapes('off');
INSERT INTO al SELECT 9000 + i, i % 7, ARRAY[i], '{}', ARRAY[i::numeric], ARRAY[[i, i]], '{1,2}' FROM generate_series(1, 100) i;
UPDATE al SET ta = NULL, ia = '{}' WHERE id % 17 = 1;
DELETE FROM al WHERE id % 19 = 3;
SELECT * FROM shapes('off buffer');
SELECT sift.col_promote('al_col');
VACUUM al;
SELECT sift.col_merge('al_col', true);
SELECT sift.col_verify('al_col', true);
SELECT * FROM shapes('off merged');
ALTER INDEX al_col SET (array_lengths = auto);
REINDEX INDEX al_col;
SELECT count(*) AS leaves_after_auto FROM sift.col_leaves('al_col');
SELECT option_value AS array_lengths FROM pg_options_to_table((SELECT reloptions FROM pg_class WHERE oid = 'al_col'::regclass)) WHERE option_name = 'array_lengths';
SELECT * FROM shapes('auto');

-- no array column: no lengths are written; a range and an inet have the lanes of their containment operators (id 1 + range 3 + inet 2)
CREATE TABLE ar (id int, r int4range, i inet) WITH (autovacuum_enabled = false);
INSERT INTO ar SELECT i, int4range(i, i + 5), ('10.0.0.' || i % 250)::inet FROM generate_series(1, 300) i;
CREATE INDEX ar_col ON ar USING sift_col (id, r, i);
SELECT count(*) AS leaves FROM sift.col_leaves('ar_col');
SELECT coalesce((SELECT option_value FROM pg_options_to_table(reloptions) WHERE option_name = 'array_lengths'), 'unset') AS array_lengths FROM pg_class WHERE oid = 'ar_col'::regclass;

DROP TABLE ar;
DROP TABLE al;
DROP FUNCTION chk(text), shapes(text);
DROP EXTENSION pg_sift;
