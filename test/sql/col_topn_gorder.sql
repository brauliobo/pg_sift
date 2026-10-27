-- ORDER BY column LIMIT n over the global order of the column: the scan merges the sorted arrays of the segments
-- and of the write buffer and fetches n rows of the heap, which confirms each entry as a b-tree's entry is confirmed. The rows are the heap's, in the heap's order with the
-- addresses of the rows as the tie-break, over NULLs, ties, DESC, NULLS FIRST and LAST, deleted and updated rows, rows of the write buffer, a promotion and a merge;
-- a segment without the array, an OFFSET, a second key or a filter on another column keep the scan that reads the units. Conditions of the query on the key bound the part of
-- the array that is read (a range, an equality, the hull of an OR), and the heap's rows are tested against them.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE TABLE gt (id int, a int2, b int, c int8, d date, t timestamp, z timestamptz, pad text DEFAULT repeat('x', 200));
ALTER TABLE gt ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gt (id, a, b, c, d, t, z)
SELECT i,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE (i * 7) % 60 - 30 END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE (i * 7919) % 500 END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE ((i * 104729) % 100003)::int8 * 5000000 - 200000000000 END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE DATE '1990-01-01' + (i * 13) % 200 END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE TIMESTAMP '2000-01-01' + ((i * 7919) % 900000) * interval '1 second' END,
	   CASE WHEN i % 47 = 0 THEN NULL ELSE TIMESTAMPTZ '2000-01-01 00:00:00+00' + ((i * 104729) % 900000) * interval '1 second' END
FROM generate_series(1, 12000) i;
CREATE INDEX gt_i ON gt USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, b sift.col_int4_ops, c sift.col_int8_ops, d sift.col_date_ops, t sift.col_timestamp_ops,
	z sift.col_timestamptz_ops) WITH (order_columns = 'id, a, b, c, d, t, z', order_array_columns = 'id, a, b, c, d, t, z');
ANALYZE gt;

-- an answer is right when its keys are the heap's, in order, and every row it names is a distinct row of the table with that key (rows of a tie may come in any order);
-- the plan says whether the scan read the global order
CREATE FUNCTION gt_chk(col text, dir text, nl text, lim int, other text DEFAULT '') RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text; pl text; used bool := false; ka text; kb text; bad bigint;
BEGIN
	q := format('SELECT id, %1$s AS k FROM gt %5$s ORDER BY %1$s %2$s NULLS %3$s LIMIT %4$s', col, dir, nl, lim, other);
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	DROP TABLE IF EXISTS pg_temp.gt_ra;
	EXECUTE 'CREATE TEMP TABLE gt_ra AS SELECT row_number() OVER () AS ord, * FROM (' || q || ') r';
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	SELECT string_agg(coalesce(k::text, 'N'), ',' ORDER BY ord) INTO ka FROM gt_ra;
	EXECUTE format('SELECT string_agg(coalesce(k::text, ''N''), '','') FROM (SELECT %1$s AS k FROM gt %5$s ORDER BY %1$s %2$s NULLS %3$s LIMIT %4$s) r', col, dir, nl, lim, other) INTO kb;
	EXECUTE format('SELECT count(*) FROM gt_ra r WHERE NOT EXISTS (SELECT 1 FROM gt WHERE gt.id = r.id AND gt.%1$s IS NOT DISTINCT FROM r.k)', col) INTO bad;
	RETURN CASE WHEN ka IS DISTINCT FROM kb OR bad > 0 OR (SELECT count(DISTINCT id) <> count(*) FROM gt_ra) THEN 'DIFFERS ' || col || ' ' || dir || ' ' || nl || ' ' || lim
				ELSE (CASE WHEN used THEN 'global order' ELSE 'units' END) END;
END $$;

CREATE FUNCTION gt_all(lims int[]) RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gt_chk(c, d, nl, l), 1::bigint FROM unnest(ARRAY['id', 'a', 'b', 'c', 'd', 't', 'z']) c, unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['FIRST', 'LAST']) nl, unnest(lims) l $$;
-- a LIMIT of 32 rows or fewer is read from the heads of the sorted runs, which the array opens and decodes a chunk of each segment to answer; the tests below turn the heads off
-- to read the array at every limit
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 32]) GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM gt_all(ARRAY[33, 200, 2000]) GROUP BY 1 ORDER BY 1;
SET sift.test_topn_heads = off;
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 200, 2000]) GROUP BY 1 ORDER BY 1;

-- the planner prices the read of the array as what it is: with the units dear to read (sift.plan_scale), a shape the array answers costs about the start of the scan, any other the
-- units it reads; 'array' is a priced read that the scan did, 'units' a scan priced as one, 'MISPRICED' a read priced as the array that the scan did not do
CREATE FUNCTION gt_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; cheap bool := false; used bool := false; agg bool := false;
BEGIN
	SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_seqscan = off; SET LOCAL sift.plan_scale = 1000;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS on, TIMING off, SUMMARY off) ' || q LOOP
		agg := agg OR pl LIKE '%Custom Scan (SiftAgg)%';
		IF pl LIKE '%Custom Scan (SiftAgg)%' THEN cheap := substring(pl from 'cost=([0-9.]+)\.\.')::float8 < 1000; END IF;
		used := used OR pl LIKE '%Global Order%';
	END LOOP;
	RETURN CASE WHEN NOT agg THEN 'native' WHEN used AND cheap THEN 'array' WHEN NOT used AND NOT cheap THEN 'units' ELSE 'MISPRICED' END;
END $$;
SELECT gt_plan(q) AS plan, q FROM unnest(ARRAY[
 'SELECT id, b FROM gt ORDER BY b LIMIT 10',
 'SELECT id, b FROM gt WHERE b >= 100 ORDER BY b LIMIT 10',
 'SELECT id, b FROM gt WHERE b < 100 ORDER BY b DESC LIMIT 10',
 'SELECT id, b FROM gt WHERE b = 100 AND a > 0 ORDER BY b, id LIMIT 10',
 'SELECT id, b FROM gt WHERE (b < 20 OR b > 480) ORDER BY b LIMIT 10',
 'SELECT id, b FROM gt WHERE a > 0 ORDER BY b LIMIT 10',
 'SELECT id, b FROM gt WHERE b > 100 AND a > 0 ORDER BY b LIMIT 10',
 'SELECT id, b FROM gt WHERE b >= 100 ORDER BY b LIMIT 10 OFFSET 5',
 'SELECT id, b FROM gt WHERE b IS NOT NULL ORDER BY b LIMIT 10',
 'SELECT id, b FROM gt WHERE b <> 100 ORDER BY b LIMIT 10'
]) q;
DROP FUNCTION gt_plan(text);

-- min(x) and max(x) are the first row, in the order of x, of the rows where x is not NULL: the scan reads the array for it, and the answer is the heap's whatever the type, the
-- NULLs, the ties, a range or a filter on another column (which the units answer)
CREATE FUNCTION gt_mm(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_seqscan = off; SET LOCAL sift.plan_scale = 1000;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	EXECUTE q INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
	EXECUTE q INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q || ' ' || coalesce(ra, 'null') || ' vs ' || coalesce(rb, 'null') WHEN used THEN 'array' ELSE 'units' END;
END $$;
CREATE FUNCTION gt_mm_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT gt_mm(format('SELECT %s(%s) FROM gt WHERE %s', f, c, w)), count(*)
   FROM unnest(ARRAY['min', 'max']) f, unnest(ARRAY['id', 'a', 'b', 'c', 'd', 't', 'z']) c,
		unnest(ARRAY['true', 'b > 100', 'b < 100', 'b = 77', 'b BETWEEN 100 AND 120', 'b > 1000', 'a > 0']) w
   GROUP BY 1 ORDER BY 1 $$;
SELECT * FROM gt_mm_all();

-- rows after the index: the write buffer joins the merge, deleted rows and rows of other versions are skipped
INSERT INTO gt (id, a, b, c, d, t, z) SELECT 12000 + i, (i * 3) % 40, NULLIF(i % 17, 0), i::int8 * 1000, DATE '2021-01-01' + i % 300, TIMESTAMP '2021-01-01' + i * interval '1 minute', NULL FROM generate_series(1, 1500) i;
DELETE FROM gt WHERE id % 11 = 0;
UPDATE gt SET a = coalesce(a, 0) + 1, b = coalesce(b, 0) + 1, c = coalesce(c, 0) + 1, d = d + 1, t = t + interval '1 day', z = z + interval '1 day' WHERE id % 7 = 0;
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 200, 2000]) GROUP BY 1 ORDER BY 1;
VACUUM gt;
SELECT * FROM gt_mm_all();
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 200, 2000]) GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('gt_i'::regclass) >= 0 AS promoted;
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 200, 2000]) GROUP BY 1 ORDER BY 1;
SELECT sift.col_merge('gt_i'::regclass, true) >= 0 AS merged;
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 200, 2000]) GROUP BY 1 ORDER BY 1;

-- the rows that came after the last segment, in the write buffer only, are in the order too
INSERT INTO gt (id, a, b, c, d, t, z) SELECT 20000 + i, -i, i, -i, DATE '1980-01-01' + i, TIMESTAMP '1980-01-01' + i * interval '1 hour', TIMESTAMPTZ '1980-01-01+00' + i * interval '1 hour' FROM generate_series(1, 50) i;
SELECT result, count(*) FROM gt_all(ARRAY[1, 10, 200, 2000]) GROUP BY 1 ORDER BY 1;

-- a write buffer of more rows than the scan sorts for so few (4,096, or 8 times the rows wanted) is read as units; its promotion gives the order back
INSERT INTO gt (id, a, b, c, d, t, z) SELECT 40000 + i, i % 90, i, i, DATE '2022-01-01' + i % 100, TIMESTAMP '2022-01-01' + i * interval '1 second', NULL FROM generate_series(1, 5000) i;
SELECT gt_chk('b', 'ASC', 'LAST', 100) AS big_buffer, gt_chk('b', 'ASC', 'LAST', 2000) AS buffer_under_8_times;
SELECT sift.col_promote('gt_i'::regclass) >= 0 AS promoted;
SELECT gt_chk('b', 'ASC', 'LAST', 100) AS promoted_buffer;

-- conditions on the key: the entries of the range only are read, whatever the types, the directions, the NULL order and the ends of the type
SELECT result, count(*) FROM (
	SELECT gt_chk(c, d, nl, l, 'WHERE ' || w) AS result
	FROM (VALUES ('id', 'id > 11990'), ('id', 'id <= 40'), ('id', 'id = 777'), ('a', 'a >= -5 AND a < 5'), ('a', 'a > 0 OR a < -28'), ('a', 'a < -1000'),
				 ('b', 'b > 1000'), ('b', 'b > 10 AND b < 5'), ('b', 'b <> 3 AND b < 6'), ('b', 'b BETWEEN 100 AND 120'), ('b', 'b <= 7 OR b >= 490 OR b = 250'),
				 ('c', 'c > 100000000000'), ('c', 'c <> 0 AND c < -190000000000'), ('c', 'c > 9223372036854775800'), ('c', 'c < -9223372036854775807 - 1'),
				 ('d', 'd >= DATE ''1990-03-01'' AND d < DATE ''1990-03-10'''), ('d', 'd = DATE ''1990-02-01'''), ('d', 'd <> DATE ''1990-02-01'''),
				 ('t', 't BETWEEN ''2000-03-01'' AND ''2000-03-02'''), ('t', 't > ''2000-01-10'' AND t <= ''2000-01-10 01:00'' OR t = ''2000-02-02'''),
				 ('z', 'z < ''2000-01-02+00''')) q(c, w),
		 unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['FIRST', 'LAST']) nl, unnest(ARRAY[1, 10, 200]) l) r
GROUP BY 1 ORDER BY 1;

-- more keys: the array gives the rows in the order of the first key, the groups of rows that share a value of it are put in order by the keys that follow, which the heap row
-- holds, until the groups make the n rows; a page after a key (a row comparison, or its expansion) reads the array from the key
CREATE FUNCTION gt_ks(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN used THEN 'global order' ELSE 'units' END;
END $$;
SELECT result, count(*) FROM (
	SELECT gt_ks(format('SELECT id, %1$s FROM gt %2$s ORDER BY %1$s %3$s, id %4$s LIMIT %5$s', c, w, d, d2, l)) AS result
	FROM (VALUES ('b', ''), ('b', 'WHERE (b, id) > (250, 5000)'), ('b', 'WHERE b > 250 OR (b = 250 AND id > 5000)'), ('b', 'WHERE (b, id) <= (40, 20000)'), ('b', 'WHERE b = 77'),
				 ('a', ''), ('a', 'WHERE (a, id) >= (-3, 900)'), ('d', ''), ('d', 'WHERE (d, id) > (DATE ''1990-03-01'', 4000)'),
				 ('t', 'WHERE (t, id) > (TIMESTAMP ''2000-03-01'', 1)'), ('c', 'WHERE c > 0 AND id > 10000')) q(c, w),
		 unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['ASC', 'DESC']) d2, unnest(ARRAY[1, 10, 50, 400]) l) r
GROUP BY 1 ORDER BY 1;
DROP FUNCTION gt_ks(text);

-- shapes that read the units: an OFFSET, a filter on another column
SELECT gt_chk('b', 'ASC', 'LAST', 50, 'WHERE a > 0');
SET sift.test_topn_gorder = off;
SELECT result, count(*) FROM gt_all(ARRAY[10, 200]) GROUP BY 1 ORDER BY 1;
RESET sift.test_topn_gorder;

-- a segment without the array (an index built without the option and then given it): the scan reads the units
CREATE INDEX gt_m ON gt USING sift_col (id sift.col_int4_ops, b sift.col_int4_ops) WITH (order_columns = 'id, b');
DROP INDEX gt_i;
ALTER INDEX gt_m SET (order_array_columns = 'id, b');
INSERT INTO gt (id, b) SELECT 30000 + i, i FROM generate_series(1, 100) i;
SELECT sift.col_promote('gt_m'::regclass) >= 0 AS promoted;
SELECT gt_chk('b', 'ASC', 'LAST', 100) AS mixed_segments, gt_chk('id', 'DESC', 'LAST', 10) AS mixed_id;
REINDEX INDEX gt_m;
SELECT gt_chk('b', 'ASC', 'LAST', 100) AS reindexed, gt_chk('id', 'DESC', 'LAST', 10) AS reindexed_id;

RESET sift.test_topn_heads;
-- a key_cut index (a unit for each range of the key, thousands of them): the array answers the small LIMITs the heads of the runs would read a unit for
DROP INDEX gt_m;
CREATE INDEX gt_k ON gt USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, t sift.col_timestamp_ops) WITH (key_cut = 'a', order_columns = 't', order_array_columns = 't');
SELECT gt_chk('t', 'DESC', 'LAST', 1) AS keycut_1, gt_chk('t', 'DESC', 'LAST', 10) AS keycut_10, gt_chk('t', 'ASC', 'FIRST', 32) AS keycut_32, gt_chk('t', 'DESC', 'LAST', 500) AS keycut_500;

DROP TABLE gt CASCADE;
DROP FUNCTION gt_chk(text, text, text, int, text), gt_all(int[]), gt_mm(text), gt_mm_all();
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
