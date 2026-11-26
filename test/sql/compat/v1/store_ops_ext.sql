-- The order, jsonb and preagg sub-vtables of Format 1 (core/store/ops.h, col/v1_engine/store_ops_ext.c and v1_store_ops_jsonb.c), reached as the engine reaches them, through the registry: what the
-- owner says (no source order, the lead key a keyed index is cut by, no key leaf of a document, the stored cube), the rows scan_rows keeps for an ORDER BY .. LIMIT of one column against the heap's
-- own, the groups unnest_run makes of a LATERAL unnest of an array (or jsonb_to_recordset) column against the heap's own, NULL, empty and NULL-element arrays and a document that is no array included,
-- and the source opened on a match set of heap addresses.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
\set VERBOSITY terse
SET sift.test_create_format1 = on;

CREATE FUNCTION sift._test_v1_ops(rel regclass, is_table bool, att int4, OUT has_ops bool, OUT sorted_by bool, OUT lead_att int4, OUT key_leaf int4, OUT has_unnest bool, OUT cube_dim_att int4, OUT cube_measure_att int4)
	RETURNS record AS '$libdir/pg_sift', 'sift_test_v1_ops' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_v1_topn(rel regclass, is_table bool, att int4, "desc" bool, nulls_first bool, k int4, qual_gt int8)
	RETURNS TABLE (rank int4, key text) AS '$libdir/pg_sift', 'sift_test_v1_topn' LANGUAGE C;
CREATE FUNCTION sift._test_v1_unnest(rel regclass, is_table bool, att int4, group_att int4, "left" bool, recordset bool)
	RETURNS TABLE (g text, a text, b text, n int8) AS '$libdir/pg_sift', 'sift_test_v1_unnest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_v1_match(rel regclass, is_table bool, att int4, match tid[])
	RETURNS TABLE (batch_val int4, fetched int4) AS '$libdir/pg_sift', 'sift_test_v1_match' LANGUAGE C STRICT;

-- the rows missing on either side (-1: the heap's answer has no row, so that nothing was compared)
CREATE FUNCTION diff(got text, want text) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE d int8; n int8;
BEGIN
	EXECUTE format('SELECT count(*) FROM (%s) w', want) INTO n;
	IF n = 0 THEN RETURN -1; END IF;
	EXECUTE format('SELECT count(*) FROM ((%1$s EXCEPT ALL %2$s) UNION ALL (%2$s EXCEPT ALL %1$s)) x', got, want) INTO d;
	RETURN d;
END $$;

-- scan_rows keeps the heap's first k keys of col in order (ties by key: the rows of a tie are any of them): 'ok' and how many rows
CREATE FUNCTION topn(rel regclass, is_table bool, att int4, tbl regclass, col text, "desc" bool, nf bool, k int4, gt int8 DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
DECLARE ok text;
BEGIN
	EXECUTE format('SELECT CASE WHEN (SELECT array_agg(key ORDER BY rank) FROM sift._test_v1_topn(%1$L, %2$L, %3$s, %4$L, %5$L, %6$s, %7$L))
		IS NOT DISTINCT FROM (SELECT array_agg(c::text) FROM (SELECT %8$I c FROM %9$s %10$s ORDER BY %8$I %11$s NULLS %12$s LIMIT %6$s) h)
		THEN ''ok '' || (SELECT count(*) FROM sift._test_v1_topn(%1$L, %2$L, %3$s, %4$L, %5$L, %6$s, %7$L)) ELSE ''DIFFERENT'' END',
		rel, is_table, att, "desc", nf, k, gt, col, tbl, CASE WHEN gt IS NULL THEN '' ELSE format('WHERE %I > %s', col, gt) END,
		CASE WHEN "desc" THEN 'DESC' ELSE 'ASC' END, CASE WHEN nf THEN 'FIRST' ELSE 'LAST' END) INTO ok;
	RETURN ok;
END $$;

-- 1. A sift_col index of Format 1 over a heap: what it hands over
CREATE TABLE c1 (id int, g int, big int8, s text, v int);
INSERT INTO c1 SELECT i, i % 17, (i * 7919) % 100003, 'k' || (i % 211) || repeat('x', i % 5), CASE WHEN i % 13 = 0 THEN NULL ELSE i % 37 END FROM generate_series(1, 6000) i;
CREATE INDEX c1_idx ON c1 USING sift_col (id, g, big, s, v) WITH (preagg = 'cube(g: big, v)');
CREATE INDEX c1_cut ON c1 USING sift_col (g, id, big, s, v) WITH (key_cut = 'g', preagg = 'off');
SELECT sift._test_index_format('c1_idx') AS idx_format, sift._test_index_format('c1_cut') AS cut_format;
SELECT * FROM sift._test_v1_ops('c1_idx', false, 1);
SELECT * FROM sift._test_v1_ops('c1_cut', false, 1);

-- 2. The rows of an ORDER BY .. LIMIT of one column, as the heap gives them
SELECT topn('c1_idx', false, 1, 'c1', 'id', false, false, 25) AS id_asc;
SELECT topn('c1_idx', false, 1, 'c1', 'id', true, true, 25) AS id_desc;
SELECT topn('c1_idx', false, 2, 'c1', 'g', true, false, 100) AS ties_desc;
SELECT topn('c1_idx', false, 3, 'c1', 'big', false, false, 40, 90000) AS big_qual;
SELECT topn('c1_idx', false, 4, 'c1', 's', false, false, 30) AS text_asc;
SELECT topn('c1_idx', false, 4, 'c1', 's', true, true, 30) AS text_desc;
SELECT topn('c1_idx', false, 5, 'c1', 'v', false, true, 50) AS nulls_first;
SELECT topn('c1_idx', false, 5, 'c1', 'v', true, false, 50) AS nulls_last;
SELECT topn('c1_idx', false, 1, 'c1', 'id', false, false, 7000) AS more_than_rows;
SELECT topn('c1_idx', false, 1, 'c1', 'id', false, false, 5, 100000) AS no_row_passes;

-- 3. After writes: new rows, new versions, dead rows
INSERT INTO c1 SELECT i, i % 17, i, 'new' || i, i % 5 FROM generate_series(6001, 6500) i;
UPDATE c1 SET id = id + 100000 WHERE id % 50 = 0;
DELETE FROM c1 WHERE id % 11 = 0 AND id < 3000;
SELECT topn('c1_idx', false, 1, 'c1', 'id', true, false, 30) AS id_desc_after_writes;
SELECT topn('c1_idx', false, 1, 'c1', 'id', false, false, 30, 2900) AS id_asc_after_writes;
SELECT topn('c1_idx', false, 4, 'c1', 's', false, false, 30) AS text_after_writes;

-- 4. A sift_table of Format 1
CREATE TABLE t1 (id int, g int, s text, v int) USING sift_table;
INSERT INTO t1 SELECT i, i % 9, 'r' || (i % 100), CASE WHEN i % 7 = 0 THEN NULL ELSE i END FROM generate_series(1, 5000) i;
SELECT * FROM sift._test_v1_ops('t1', true, 1);
SELECT topn('t1', true, 1, 't1', 'id', true, false, 20) AS tbl_id_desc;
SELECT topn('t1', true, 3, 't1', 's', false, false, 20) AS tbl_text;
SELECT topn('t1', true, 4, 't1', 'v', false, true, 20, 100) AS tbl_nulls_qual;
DELETE FROM t1 WHERE id % 3 = 0;
SELECT topn('t1', true, 1, 't1', 'id', false, false, 20) AS tbl_after_delete;

-- 5. A LATERAL unnest of an array column, as the heap takes it apart: NULL and empty arrays, NULL elements, repeats
CREATE TABLE u1 (id int, g int, arr int4[], tags text[], doc jsonb);
INSERT INTO u1 SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL WHEN i % 5 = 0 THEN '{}' ELSE ARRAY[i % 10, (i + 3) % 10, CASE WHEN i % 4 = 0 THEN NULL ELSE i % 3 END] END,
	CASE WHEN i % 13 = 0 THEN NULL WHEN i % 6 = 0 THEN '{}' ELSE ARRAY['t' || i % 4, 'u' || i % 9, 'v' || i % 4] END,
	CASE WHEN i % 17 = 0 THEN NULL WHEN i % 8 = 0 THEN '[]' ELSE jsonb_build_array(jsonb_build_object('a', i % 6, 'b', 'x' || i % 3), jsonb_build_object('a', (i + 1) % 6, 'b', 'y' || i % 5), jsonb_build_object('a', NULL, 'b', 'z')) END
	FROM generate_series(1, 3000) i;
CREATE INDEX u1_idx ON u1 USING sift_col (id, g, arr, tags, doc);
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 3, 0, false, false)', 'SELECT NULL::text, e::text, NULL::text, count(*) FROM u1, LATERAL unnest(arr) e GROUP BY e') AS int_arrays;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 3, 2, false, false)', 'SELECT g::text, e::text, NULL::text, count(*) FROM u1, LATERAL unnest(arr) e GROUP BY g, e') AS int_arrays_by_group;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 3, 2, true, false)', 'SELECT g::text, e::text, NULL::text, count(*) FROM u1 LEFT JOIN LATERAL unnest(arr) e ON true GROUP BY g, e') AS int_arrays_left;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 4, 2, false, false)', 'SELECT g::text, e::text, NULL::text, count(*) FROM u1, LATERAL unnest(tags) e GROUP BY g, e') AS text_arrays_by_group;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 4, 2, true, false)', 'SELECT g::text, e::text, NULL::text, count(*) FROM u1 LEFT JOIN LATERAL unnest(tags) e ON true GROUP BY g, e') AS text_arrays_left;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 5, 2, false, true)', 'SELECT g::text, r.a::text, r.b, count(*) FROM u1, LATERAL jsonb_to_recordset(doc) AS r(a int, b text) GROUP BY g, r.a, r.b') AS recordset;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 5, 2, true, true)', 'SELECT g::text, r.a::text, r.b, count(*) FROM u1 LEFT JOIN LATERAL jsonb_to_recordset(doc) AS r(a int, b text) ON true GROUP BY g, r.a, r.b') AS recordset_left;

-- 6. After writes, and a sift_table
UPDATE u1 SET arr = arr || 99 WHERE id % 9 = 0;
DELETE FROM u1 WHERE id % 10 = 3;
INSERT INTO u1 SELECT i, i % 7, ARRAY[i % 10, 1], ARRAY['n' || i % 3], '[{"a": 1, "b": "late"}]' FROM generate_series(3001, 3300) i;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 3, 2, false, false)', 'SELECT g::text, e::text, NULL::text, count(*) FROM u1, LATERAL unnest(arr) e GROUP BY g, e') AS int_arrays_after_writes;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u1_idx'', false, 5, 2, true, true)', 'SELECT g::text, r.a::text, r.b, count(*) FROM u1 LEFT JOIN LATERAL jsonb_to_recordset(doc) AS r(a int, b text) ON true GROUP BY g, r.a, r.b') AS recordset_after_writes;
CREATE TABLE u2 (id int, g int, arr int4[]) USING sift_table;
INSERT INTO u2 SELECT id, g, arr FROM u1 WHERE id <= 2000;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u2'', true, 3, 2, true, false)', 'SELECT g::text, e::text, NULL::text, count(*) FROM u2 LEFT JOIN LATERAL unnest(arr) e ON true GROUP BY g, e') AS table_arrays_left;

-- 7. A document that is no array: the error is the function's own, on both sides
CREATE TABLE u3 (id int, doc jsonb);
INSERT INTO u3 SELECT i, '[{"a": 1, "b": "p"}]' FROM generate_series(1, 100) i;
INSERT INTO u3 VALUES (101, '{"a": 1}');
CREATE INDEX u3_idx ON u3 USING sift_col (id, doc);
SELECT count(*) FROM sift._test_v1_unnest('u3_idx', false, 2, 1, false, true);
SELECT count(*) FROM u3, LATERAL jsonb_to_recordset(doc) AS r(a int, b text);
DELETE FROM u3 WHERE id = 101;
SELECT diff('SELECT g, a, b, n FROM sift._test_v1_unnest(''u3_idx'', false, 2, 1, false, true)', 'SELECT id::text, r.a::text, r.b, count(*) FROM u3, LATERAL jsonb_to_recordset(doc) AS r(a int, b text) GROUP BY id, r.a, r.b') AS poison_gone;

-- 8. The source on a match set of heap addresses gives those rows
CREATE FUNCTION match_diff(rel regclass, is_table bool, tbl regclass, pred text) RETURNS int8 LANGUAGE plpgsql AS $$
DECLARE m tid[];
BEGIN
	SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT array_agg(ctid) FROM %s WHERE %s', tbl, pred) INTO m;
	RETURN diff(format('SELECT batch_val, fetched FROM sift._test_v1_match(%L, %L, 1, %L)', rel, is_table, m), format('SELECT id, id FROM %s WHERE %s', tbl, pred));
END $$;
SELECT match_diff('c1_idx', false, 'c1', 'id % 97 = 0') AS match_set;
SELECT match_diff('c1_idx', false, 'c1', 'id > 6400 OR id < 5') AS match_set_edges;
SELECT match_diff('t1', true, 't1', 'id % 97 = 0') AS match_set_table;

-- 9. The aggregate scan of a LATERAL unnest of a sift_table of Format 1 answers as the heap does: the quals of the table, NULL and empty arrays, a LEFT JOIN
SET max_parallel_workers_per_gather = 0; SET seq_page_cost = 10000; SET enable_hashagg = off; SET sift.agg_force = on;
CREATE TABLE u5 (id int, g text, arr int4[]) USING sift_table;
INSERT INTO u5 SELECT i, 'g' || i % 3, CASE WHEN i % 4 = 0 THEN NULL WHEN i % 4 = 1 THEN ARRAY[i, i + 1] WHEN i % 4 = 2 THEN '{}' ELSE ARRAY[i] END FROM generate_series(1, 2000) i;
CREATE TABLE u5h (LIKE u5);
INSERT INTO u5h SELECT * FROM u5;
CREATE FUNCTION scanned(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		IF l LIKE '%Unnest: %' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT scanned('SELECT g, count(*), sum(e) FROM u5 LEFT JOIN LATERAL unnest(arr) e ON true WHERE id >= 100 GROUP BY g') AS table_unnest_in_the_plan;
SELECT diff('SELECT g, count(*), sum(e) FROM u5 LEFT JOIN LATERAL unnest(arr) e ON true WHERE id >= 100 GROUP BY g', 'SELECT g, count(*), sum(e) FROM u5h LEFT JOIN LATERAL unnest(arr) e ON true WHERE id >= 100 GROUP BY g') AS table_unnest_left_where;
SELECT diff('SELECT g, count(*), sum(e) FROM u5, unnest(arr) e WHERE id < 700 AND g <> ''g1'' GROUP BY g', 'SELECT g, count(*), sum(e) FROM u5h, unnest(arr) e WHERE id < 700 AND g <> ''g1'' GROUP BY g') AS table_unnest_where;
SELECT diff('SELECT g, count(*), sum(e) FROM u5 LEFT JOIN LATERAL unnest(arr) e ON true GROUP BY g', 'SELECT g, count(*), sum(e) FROM u5h LEFT JOIN LATERAL unnest(arr) e ON true GROUP BY g') AS table_unnest_left;
DROP FUNCTION scanned(text);

DROP FUNCTION diff, topn, match_diff, sift._test_v1_ops, sift._test_v1_topn, sift._test_v1_unnest, sift._test_v1_match;
DROP TABLE c1, t1, u1, u2, u3, u5, u5h;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
