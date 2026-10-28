-- The global order of a text column: ORDER BY txt LIMIT n reads n entries of each segment, whatever the number of units, and
-- merges the segments (and the rows of the write buffer) by comparing their strings under the collation of the column. The answers are the heap's through ties, NULLs, empty
-- strings, accents, case and punctuation, both directions, NULLS FIRST and LAST, OFFSET, DML, a write buffer, a promotion, a merge and a REINDEX; a sort under another
-- collation than the column's, a text that is a unit's inline string, and an expression of the column read the units.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = notice;
\set VERBOSITY terse

CREATE TABLE tt (id int, a text, b text COLLATE "C", v varchar, k int, pad text DEFAULT repeat('x', 200));
ALTER TABLE tt ALTER COLUMN pad SET STORAGE PLAIN;
-- strings that sort differently by bytes and by a locale (case, accents, punctuation, spaces), ties, an empty string and NULLs
CREATE FUNCTION tt_str(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 11 WHEN 0 THEN '' WHEN 1 THEN 'a' ELSE '' END
		|| (ARRAY['Zeta', 'alpha', 'Alpha', 'ALPHA', 'beta-2', 'beta 2', 'Beta2', 'été', 'ete', 'Ete', 'zz', '_x', '10', '9', 'a-b', 'ab', 'A.B', 'ä', 'a', 'z'])[1 + (i * 7) % 20]
		|| (i % 97)::text $$;
INSERT INTO tt (id, a, b, v, k)
SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE tt_str(i) END, CASE WHEN i % 37 = 0 THEN NULL ELSE tt_str(i * 3) END, CASE WHEN i % 29 = 0 THEN NULL ELSE tt_str(i * 5) END, i % 100
FROM generate_series(1, 9000) i;
INSERT INTO tt (id, a, b, v) VALUES (9001, '', '', ''), (9002, NULL, NULL, NULL);
CREATE INDEX tt_i ON tt USING sift_col (id sift.col_int4_ops, a sift.col_text_ops, b sift.col_text_ops, v sift.col_text_ops, k sift.col_int4_ops) WITH (order_array_columns = 'a, b, v');
SELECT sift.col_verify('tt_i');
ANALYZE tt;

-- an answer is right when the rows are the heap's, in order; 'global order' says the scan read the arrays
CREATE FUNCTION tt_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	EXECUTE 'SELECT string_agg(coalesce(x::text, ''null''), '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(coalesce(x::text, ''null''), '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN used THEN 'global order' ELSE 'units' END;
END $$;

-- the key of every row is output, so equal strings (which come in any order) give the same answer
CREATE FUNCTION tt_all() RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT tt_chk(format('SELECT %1$s FROM tt ORDER BY %1$s %2$s %3$s LIMIT %4$s OFFSET %5$s', c, d, nl, l, o)), 1::bigint
   FROM unnest(ARRAY['a', 'b', 'v']) c, unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['NULLS FIRST', 'NULLS LAST']) nl, unnest(ARRAY[1, 10, 100, 2000]) l, unnest(ARRAY[0, 7]) o $$;
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;

-- another collation of the sort than the column's, an expression of the column, a condition, another key after the first, and no LIMIT read the units (or the normal plan)
SELECT tt_chk('SELECT a FROM tt ORDER BY a COLLATE "C" LIMIT 10') AS other_collation, tt_chk('SELECT b FROM tt ORDER BY b COLLATE "POSIX" LIMIT 10') AS posix_collation,
	   tt_chk('SELECT lower(a) FROM tt ORDER BY lower(a) LIMIT 10') AS expression, tt_chk('SELECT a FROM tt WHERE k = 5 ORDER BY a LIMIT 10') AS condition,
	   tt_chk('SELECT a, id FROM tt ORDER BY a, id LIMIT 10') AS two_keys, tt_chk('SELECT a FROM tt ORDER BY a') AS no_limit;

-- the rows of the write buffer, deletes, updates of the key, VACUUM, a promotion, a merge and a REINDEX
INSERT INTO tt (id, a, b, v, k) SELECT 20000 + i, tt_str(i * 13), tt_str(i * 17), CASE WHEN i % 3 = 0 THEN NULL ELSE tt_str(i * 19) END, i % 100 FROM generate_series(1, 700) i;
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
DELETE FROM tt WHERE id % 7 = 0;
UPDATE tt SET a = 'U' || a WHERE id % 11 = 0;
UPDATE tt SET b = NULL WHERE id % 13 = 0;
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
VACUUM tt;
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_promote('tt_i'::regclass) >= 0 AS promoted;
SELECT sift.col_verify('tt_i');
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
INSERT INTO tt (id, a, b, v, k) SELECT 30000 + i, tt_str(i * 23), tt_str(i * 29), tt_str(i * 31), i % 100 FROM generate_series(1, 300) i;
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
SELECT sift.col_merge('tt_i'::regclass, true) >= 0 AS merged;
SELECT sift.col_verify('tt_i');
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
REINDEX INDEX tt_i;
SELECT sift.col_verify('tt_i');
SELECT result, count(*) FROM tt_all() GROUP BY 1 ORDER BY 1;
-- a write buffer of more rows than the scan sorts for so few reads the units
INSERT INTO tt (id, a) SELECT 50000 + i, tt_str(i) FROM generate_series(1, 5000) i;
SELECT tt_chk('SELECT a FROM tt ORDER BY a LIMIT 10') AS big_buffer;

-- a string longer than a dictionary entry (a unit stores it inline): the column keeps no array and the answer is the heap's
CREATE TABLE tl (id int, s text);
INSERT INTO tl SELECT i, CASE WHEN i % 500 = 0 THEN repeat('long string ', 30) || i ELSE 'k' || (i * 7919) % 3000 END FROM generate_series(1, 6000) i;
CREATE INDEX tl_i ON tl USING sift_col (id sift.col_int4_ops, s sift.col_text_ops) WITH (order_array_columns = 's');
SELECT sift.col_verify('tl_i');
ANALYZE tl;
SELECT tt_chk('SELECT s FROM tl ORDER BY s LIMIT 10') AS long_strings;

-- the build chooses a text column of an index when order_array_columns is unset or 'auto': benefit log2(300) / log2(1.05) = 117 against cost G / 3 + W / 5 (the bytes of the
-- array in the share of the index, the sort of the strings in the share of the build; 62% for 4 columns under the C collation of these tests, 420% under a locale), on a table of 100,000 rows
-- or more, 1,000 or more distinct values scattered over the heap, strings up to 64 bytes. A longer string is stored again in the array, so that column is left to the list.
CREATE TABLE tn (id int, s text, w text, q int);
INSERT INTO tn SELECT i, md5(i::text), md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text), i % 100 FROM generate_series(1, 120000) i;
ANALYZE tn;
CREATE INDEX tn_i ON tn USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, w sift.col_text_ops, q sift.col_int4_ops);
SELECT reloptions FROM pg_class WHERE relname = 'tn_i';
SELECT tt_chk('SELECT s FROM tn ORDER BY s LIMIT 10') AS auto_asc, tt_chk('SELECT s FROM tn ORDER BY s DESC NULLS FIRST LIMIT 10 OFFSET 7') AS auto_desc, tt_chk('SELECT w FROM tn ORDER BY w LIMIT 10') AS wide_column;
DROP INDEX tn_i;
CREATE INDEX tn_off ON tn USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, w sift.col_text_ops, q sift.col_int4_ops) WITH (order_array_columns = 'off');
SELECT reloptions FROM pg_class WHERE relname = 'tn_off';
SELECT tt_chk('SELECT s FROM tn ORDER BY s LIMIT 10') AS off_switch;
DROP TABLE tn;

DROP TABLE tt, tl CASCADE;
DROP FUNCTION tt_chk(text), tt_all(), tt_str(int);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
