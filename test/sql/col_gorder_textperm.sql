-- The global order of a text column names the strings of its ranks by their dictionary codes (column format minor 42) and does not keep a second copy of them: the index is smaller, the
-- answers are the heap's, and the segments written with the strings (the order before minor 42) are read, merged and rewritten beside them.
CREATE EXTENSION pg_sift_test CASCADE;
SET default_statistics_target = 10000;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

-- strings that differ in case, accents and punctuation, repeat, are empty or NULL; a few thousand distinct ones, so the dictionary is stored in blocks
CREATE FUNCTION tp_str(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 13 WHEN 0 THEN '' WHEN 1 THEN 'É' WHEN 2 THEN 'a-' ELSE 'Cust ' END || substr(md5((i % 3500)::text), 1, 6 + i % 9) $$;
CREATE TABLE tp (id int, a text, b text COLLATE "C", k int, pad text DEFAULT repeat('x', 100));
ALTER TABLE tp ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO tp (id, a, b, k) SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE tp_str(i) END, CASE WHEN i % 37 = 0 THEN NULL ELSE tp_str(i * 3) END, i % 100 FROM generate_series(1, 12000) i;
CREATE TABLE ts AS SELECT * FROM tp;
CREATE TABLE tm AS SELECT * FROM tp;

-- the index with the strings, then the one with the codes
SET sift.test_col_gorder_strings = on;
CREATE INDEX ts_i ON ts USING sift_col (id sift.col_int4_ops, a sift.col_text_ops, b sift.col_text_ops, k sift.col_int4_ops) WITH (order_array_columns = 'a, b');
RESET sift.test_col_gorder_strings;
CREATE INDEX tp_i ON tp USING sift_col (id sift.col_int4_ops, a sift.col_text_ops, b sift.col_text_ops, k sift.col_int4_ops) WITH (order_array_columns = 'a, b');
SELECT 'strings' AS block, format FROM sift.col_segments('ts_i') WHERE kind = 'immutable' UNION ALL SELECT 'codes', format FROM sift.col_segments('tp_i') WHERE kind = 'immutable' ORDER BY 1, 2;
SELECT pg_relation_size('tp_i') < pg_relation_size('ts_i') AS codes_are_smaller;
SELECT sift.col_verify('ts_i');
SELECT sift.col_verify('tp_i');
ANALYZE tp;
ANALYZE ts;

-- an answer is right when the rows are the heap's, in order; 'global order' says the scan read the arrays
CREATE FUNCTION tp_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
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
CREATE FUNCTION tp_all(t text) RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT tp_chk(format('SELECT %1$s FROM %6$s ORDER BY %1$s %2$s %3$s LIMIT %4$s OFFSET %5$s', c, d, nl, l, o, t)), 1::bigint
   FROM unnest(ARRAY['a', 'b']) c, unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['NULLS FIRST', 'NULLS LAST']) nl, unnest(ARRAY[1, 20, 3000]) l, unnest(ARRAY[0, 11, 2500]) o $$;
CREATE FUNCTION tp_range(t text) RETURNS TABLE (result text, n bigint) LANGUAGE sql AS
$$ SELECT tp_chk(format('SELECT %1$s FROM %2$s WHERE %1$s >= ''%3$s'' AND %1$s < ''%4$s'' ORDER BY %1$s LIMIT 30 OFFSET %5$s', c, t, lo, hi, o)), 1::bigint
   FROM unnest(ARRAY['a', 'b']) c, unnest(ARRAY['a', 'Cust 3', 'Cust a', 'É']) lo, unnest(ARRAY['Cust 4', 'Cust b', 'z']) hi, unnest(ARRAY[0, 40]) o $$;
SELECT 'strings' AS block, result, count(*) FROM tp_all('ts') GROUP BY 2 UNION ALL SELECT 'codes', result, count(*) FROM tp_all('tp') GROUP BY 2 ORDER BY 1, 2;
SELECT 'strings' AS block, result, count(*) FROM tp_range('ts') GROUP BY 2 UNION ALL SELECT 'codes', result, count(*) FROM tp_range('tp') GROUP BY 2 ORDER BY 1, 2;

-- one table with both: the segments written with the strings, the rows of the write buffer, and the segments written with the codes, merged and rewritten
SET sift.test_col_gorder_strings = on;
CREATE INDEX tm_i ON tm USING sift_col (id sift.col_int4_ops, a sift.col_text_ops, b sift.col_text_ops, k sift.col_int4_ops) WITH (order_array_columns = 'a, b', write_buffer_limit = 65536);
RESET sift.test_col_gorder_strings;
INSERT INTO tm (id, a, b, k) SELECT 20000 + i, tp_str(i * 7), tp_str(i * 11), i % 100 FROM generate_series(1, 4000) i;
SELECT sift.col_promote('tm_i') >= 0 AS promoted;
SELECT format, count(*) AS segments FROM sift.col_segments('tm_i') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
INSERT INTO tm (id, a, b, k) SELECT 30000 + i, tp_str(i * 5), tp_str(i * 17), i % 100 FROM generate_series(1, 300) i;
ANALYZE tm;
SELECT result, count(*) FROM tp_all('tm') GROUP BY 1 ORDER BY 1;
DELETE FROM tm WHERE id % 7 = 0;
UPDATE tm SET a = 'U' || a WHERE id % 11 = 0;
SELECT result, count(*) FROM tp_all('tm') GROUP BY 1 ORDER BY 1;
SELECT sift.col_merge('tm_i', true) >= 0 AS merged;
SELECT format, count(*) AS segments FROM sift.col_segments('tm_i') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
VACUUM tm;
SELECT result, count(*) FROM tp_all('tm') GROUP BY 1 ORDER BY 1;
SELECT sift.col_verify('tm_i');
REINDEX INDEX tm_i;
SELECT format, count(*) AS segments FROM sift.col_segments('tm_i') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT result, count(*) FROM tp_all('tm') GROUP BY 1 ORDER BY 1;
SELECT sift.col_verify('tm_i');

-- a format minor below the one that has the order array writes none
SET sift.col_format_minor = 14;
REINDEX INDEX tp_i;
RESET sift.col_format_minor;
SELECT format FROM sift.col_segments('tp_i') WHERE kind = 'immutable';

DROP FUNCTION tp_chk(text), tp_all(text), tp_range(text), tp_str(int);
DROP TABLE tp, ts, tm;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
