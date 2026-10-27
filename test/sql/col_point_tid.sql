-- The point directory of heap addresses: the slots of a leaf list the addresses of the rows, so
-- an equality or a short IN list of the access method returns them with no unit read, and the heap checks every condition again. The answers are the heap's over NULLs,
-- duplicates, text and uuid, DML, VACUUM (a unit with a dead row is read instead: its addresses may be used again), the write buffer, a merge, and an index
-- of the rows-of-the-segment form (minor 14), which is read as before and upgraded by REINDEX.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE pt (id int, k int, s text, u uuid, pad text DEFAULT repeat('x', 400));
ALTER TABLE pt ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pt (id, k, s, u)
SELECT i,
	   CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7919) % 9000 END,
	   CASE WHEN i % 47 = 0 THEN NULL WHEN i % 41 = 0 THEN '' ELSE 's' || (i * 31) % 6000 END,
	   CASE WHEN i % 59 = 0 THEN NULL ELSE md5((i % 5000)::text)::uuid END
FROM generate_series(1, 30000) i;

-- the answer of the access method by an index scan, by a bitmap scan, and of the heap
CREATE FUNCTION pt_idx(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pt WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pt_bmp(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_indexscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pt WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pt_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pt WHERE %s', w) INTO r;
	RETURN r;
END $$;
-- the rows of a key list the directory returns, none missing (the fingerprint of a key may bring a few more, the heap drops them)
CREATE FUNCTION pt_cover(idx regclass, preds text, heapwhere text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE mi bigint; extra bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM pt WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L, 24)) z', heapwhere, idx, preds) INTO mi;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L, 24) EXCEPT SELECT ctid FROM pt WHERE %s) z', idx, preds, heapwhere) INTO extra;
	RETURN CASE WHEN mi = 0 AND extra < 20 THEN 'ok' ELSE format('MISMATCH missing=%s extra=%s', mi, extra) END;
END $$;

-- leaves: 0 is k, 1 is s, 2 is u, 3 is id
CREATE TABLE pt_q (n serial, preds text, heapwhere text);
INSERT INTO pt_q (preds, heapwhere) VALUES
 ('0:eq:4321', 'k = 4321'),
 ('0:eq:0', 'k = 0'),
 ('0:eq:99999', 'k = 99999'),
 ('0:in:1|2|3|4000|8999', 'k IN (1, 2, 3, 4000, 8999)'),
 ('0:in:5|5|5', 'k IN (5, 5, 5)'),
 ('1:eq:s77', 's = ''s77'''),
 ('1:eq:', 's = '''''),
 ('1:eq:nothing', 's = ''nothing'''),
 ('1:in:s1|s2|s3|none', 's IN (''s1'', ''s2'', ''s3'', ''none'')'),
 ('2:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 'u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b'''),
 ('2:in:c4ca4238-a0b9-2382-0dcc-509a6f75849b|c81e728d-9d4c-2f63-6f06-7f89cc14862c', 'u IN (''c4ca4238-a0b9-2382-0dcc-509a6f75849b'', ''c81e728d-9d4c-2f63-6f06-7f89cc14862c'')'),
 ('2:eq:00000000-0000-0000-0000-000000000001', 'u = ''00000000-0000-0000-0000-000000000001''');
CREATE TABLE pt_w (n serial, w text);
INSERT INTO pt_w (w) VALUES
 ('k = 4321'), ('k = 99999'), ('k IN (1, 2, 3, 4000, 8999)'), ('k IS NULL'), ('k = 4321 AND id > 15000'), ('k BETWEEN 100 AND 103'), ('k BETWEEN 100 AND 300'),
 ('s = ''s77'''), ('s IN (''s1'', ''s2'', ''none'')'), ('s = '''''), ('u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b'''),
 ('u IN (''c4ca4238-a0b9-2382-0dcc-509a6f75849b'', ''c81e728d-9d4c-2f63-6f06-7f89cc14862c'')'), ('k = 4321 AND s = ''s77'''), ('k = 4321 OR s = ''s77'''), ('id = 777');
-- long IN lists (65 keys and more, hits, misses and a NULL among them): the directory answers up to 65,536 keys, and not a list that names an eighteenth of the rows
INSERT INTO pt_w (w)
SELECT format('k IN (%s)', string_agg(((i * 7919) % 9000 + (i % 3) * 20000)::text, ',' ORDER BY i)) FROM generate_series(1, 65) i
UNION ALL SELECT format('k IN (%s)', string_agg(((i * 7919) % 9000 + (i % 3) * 20000)::text, ',' ORDER BY i)) FROM generate_series(1, 300) i
UNION ALL SELECT format('k IN (%s)', string_agg(((i * 7919) % 9000 + (i % 3) * 20000)::text, ',' ORDER BY i)) FROM generate_series(1, 2500) i
UNION ALL SELECT format('s IN (%s)', string_agg(quote_literal('s' || (i * 31) % 6500), ',' ORDER BY i)) FROM generate_series(1, 200) i
UNION ALL SELECT format('u IN (%s)', string_agg(quote_literal(md5((i % 6000)::text)::uuid), ',' ORDER BY i)) FROM generate_series(1, 100) i
UNION ALL SELECT format('id IN (%s)', string_agg((i * 17 % 31000)::text, ',' ORDER BY i)) FROM generate_series(1, 1000) i;

-- the directory: the segments are minor 16, verify lists the address of every row under its key, and a lookup reads no unit of a segment without a dead row
CREATE INDEX pt_t ON pt USING sift_col (k sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, id sift.col_int4_ops) WITH (count_columns = 'off', write_buffer_limit = 256, point_tid_columns = 'k, s, u');
VACUUM (ANALYZE) pt;
SELECT sift.col_verify('pt_t', true);
SELECT count(*) > 0 AS segments, min(format) AS format, sum(units) > 2 AS units FROM sift.col_segments('pt_t') WHERE kind = 'immutable';
SELECT p.units AS units_with_addresses, f.units > 2 AS units_without, p.rows = 0 AS no_row_read
FROM sift._test_col_counters('pt_t', '0:eq:4321', 24) p, sift._test_col_counters('pt_t', '0:eq:4321', 0) f;
SELECT p.units AS units_with_addresses, p.rows = 0 AS no_row_read FROM sift._test_col_counters('pt_t', '1:in:s1|s2|s3|none', 24) p;
-- 300 keys: the units of their rows only, no row read; the same list answered by the units reads more
SELECT p.units < f.units AS fewer_units, p.rows = 0 AS no_row_read
FROM sift._test_col_counters('pt_t', '0:in:' || (SELECT string_agg(((i * 7919) % 9000)::text, '|') FROM generate_series(1, 300) i), 24) p,
	 sift._test_col_counters('pt_t', '0:in:' || (SELECT string_agg(((i * 7919) % 9000)::text, '|') FROM generate_series(1, 300) i), 0) f;
SELECT p.units AS units_with_addresses, p.rows = 0 AS no_row_read FROM sift._test_col_counters('pt_t', '2:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 24) p;
-- a leaf that has no directory, and a range, read their units as before
SELECT p.units = f.units AS same_units_for_a_range, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pt_t', '0:ge:100;0:le:300', 24) p, sift._test_col_counters('pt_t', '0:ge:100;0:le:300', 0) f;
SELECT p.units = f.units AS same_units_for_a_leaf_without, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pt_t', '3:eq:777', 24) p, sift._test_col_counters('pt_t', '3:eq:777', 0) f;
SELECT n, preds, pt_cover('pt_t', preds, heapwhere) FROM pt_q ORDER BY n;
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan, cardinality(pt_seq(w)) AS matches FROM pt_w ORDER BY n;

-- a segment with a dead row is read instead of the addresses of its rows, which a VACUUM frees to be used again: the rows of the new key are those of the heap
DELETE FROM pt WHERE k = 4321 AND id < 15000;
DELETE FROM pt WHERE s = 's77';
VACUUM (INDEX_CLEANUP on) pt;
SELECT p.units > 0 AS dirty_units_are_read, p.units <= f.units AS no_more_than_without
FROM sift._test_col_counters('pt_t', '1:eq:s77', 24) p, sift._test_col_counters('pt_t', '1:eq:s77', 0) f;
SELECT p.units > 0 AS dirty_units_are_read FROM sift._test_col_counters('pt_t', '0:eq:4321', 24) p;
SELECT n, preds, pt_cover('pt_t', preds, heapwhere) FROM pt_q WHERE n IN (1, 6) ORDER BY n;
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan, cardinality(pt_seq(w)) AS matches FROM pt_w ORDER BY n;
INSERT INTO pt (id, k, s, u) SELECT i, 4321, 's77', 'c4ca4238-a0b9-2382-0dcc-509a6f75849b' FROM generate_series(30001, 30400) i;
UPDATE pt SET k = 4321, s = 's5' WHERE id % 1999 = 0;
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan, cardinality(pt_seq(w)) AS matches FROM pt_w ORDER BY n;
SELECT (SELECT count(*) FROM (SELECT id FROM pt WHERE k = 4321) a) = (SELECT count(*) FROM (SELECT DISTINCT id FROM pt WHERE k = 4321) b) AS no_row_twice;
VACUUM (INDEX_CLEANUP on) pt;
SELECT sift.col_promote('pt_t'::regclass) >= 0 AS promoted;
SELECT sift.col_verify('pt_t', true);
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan FROM pt_w ORDER BY n;
SELECT sift.col_merge('pt_t'::regclass, true) >= 0 AS merged;
SELECT kind, format, count(*) AS segments FROM sift.col_segments('pt_t') GROUP BY kind, format ORDER BY kind, format;
SELECT sift.col_verify('pt_t', true);
SELECT n, preds, pt_cover('pt_t', preds, heapwhere) FROM pt_q ORDER BY n;
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan FROM pt_w ORDER BY n;

-- a LIKE or ^@ with a literal prefix: the dictionary names the keys with the prefix and the directory lists their rows; the planner prices it as the probes of those keys, so a count of them is the access method's
SELECT w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan, cardinality(pt_seq(w)) AS matches
FROM (VALUES ('s LIKE ''s12%'''), ('s ^@ ''s12'''), ('s LIKE ''s1%'''), ('s LIKE ''s4321'''), ('s LIKE ''%12'''), ('s LIKE ''s\_1%''')) v(w);
CREATE FUNCTION pt_plan(q text) RETURNS text LANGUAGE plpgsql SET sift.enable_col_scan = on SET max_parallel_workers_per_gather = 0 AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '^\s*(->\s*)?(Aggregate|Custom Scan|Index Scan|Index Only Scan|Bitmap Heap Scan|Bitmap Index Scan|Seq Scan)' THEN r := r || ' ' || trim(regexp_replace(l, '^\s*->\s*', '')); END IF;
	END LOOP;
	RETURN trim(r);
END $$;
SELECT pt_plan('SELECT count(*) FROM pt WHERE s LIKE ''s5999%''') AS prefix_count, pt_plan('SELECT count(*) FROM pt WHERE s ^@ ''s4321''') AS starts_with_count;
DROP FUNCTION pt_plan(text);

-- one damage at a time in the directories, through a fresh open and the check of the segment (meaningful under SANITIZE=1)
SELECT runs > 0 AS has_sections, damaged > 100 AS many, refused = damaged AS all_refused, unnoticed FROM sift._test_col_point_file_damage('pt_t', 1000000000);
SELECT runs > 0 AS has_sections, damaged > 100 AS many, refused > 0 AS some_refused FROM sift._test_col_point_file_damage('pt_t', 997);

-- the setting 14 writes the directory of rows (minor 14), the index of the rows is read as before, and a REINDEX with the option lists the addresses
SET sift.col_format_minor = 14;
CREATE INDEX pt_r ON pt USING sift_col (k sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, id sift.col_int4_ops) WITH (count_columns = 'off', point_tid_columns = 'k, s, u');
RESET sift.col_format_minor;
SELECT max(format) AS format FROM sift.col_segments('pt_r') WHERE kind = 'immutable';
SELECT sift.col_verify('pt_r', true);
DROP INDEX pt_t;
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan FROM pt_w ORDER BY n;
ALTER INDEX pt_r RESET (point_tid_columns);
ALTER INDEX pt_r SET (point_columns = 'k, s, u');
REINDEX INDEX pt_r;
SELECT max(format) AS format FROM sift.col_segments('pt_r') WHERE kind = 'immutable';
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan FROM pt_w ORDER BY n;
ALTER INDEX pt_r RESET (point_columns);
ALTER INDEX pt_r SET (point_tid_columns = 'k, s, u');
SELECT kind, object, command FROM sift.upgrade_commands() WHERE reason ~ 'point_tid_columns';
REINDEX INDEX pt_r;
SELECT count(*) AS commands FROM sift.upgrade_commands() WHERE reason ~ 'point_tid_columns';
SELECT min(format) AS format FROM sift.col_segments('pt_r') WHERE kind = 'immutable';
SELECT sift.col_verify('pt_r', true);
SELECT n, left(w, 60) AS w, pt_idx(w) = pt_seq(w) AS index_scan, pt_bmp(w) = pt_seq(w) AS bitmap_scan FROM pt_w ORDER BY n;

-- narrow rows (about 290 on a page, so the offset numbers reach the largest of a page) and many blocks: the lists are the heap's
CREATE TABLE pb (id int, k int);
INSERT INTO pb SELECT i, i % 7 FROM generate_series(1, 200000) i;
CREATE INDEX pb_t ON pb USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops) WITH (point_tid_columns = 'k');
SELECT sift.col_verify('pb_t', true);
SELECT p.units AS units_with_addresses, p.rows = 0 AS no_row_read FROM sift._test_col_counters('pb_t', '0:eq:3', 24) p;
SELECT (SELECT count(*) FROM sift._test_col_tids('pb_t', '0:eq:3', 24)) >= (SELECT count(*) FROM pb WHERE k = 3) AS all_rows_listed;
-- a key in every unit with a range on another column: the zones of the range cut the units first, and the directory lists the addresses of the kept units only
SELECT t.listed < (SELECT count(*) FROM pb WHERE k = 3) AS fewer_than_the_key, t.listed >= (SELECT count(*) FROM pb WHERE k = 3 AND id BETWEEN 100000 AND 101000) AS all_rows_of_the_range
FROM (SELECT count(*) AS listed FROM sift._test_col_tids('pb_t', '0:eq:3;1:ge:100000;1:le:101000', 24)) t;
-- several segments: the lists of the keys come segment by segment and are sorted only when the blocks of a segment start below the last of the one before (rows inserted into freed pages)
CREATE TABLE pbs (id int, k int);
INSERT INTO pbs SELECT i, i % 7 FROM generate_series(1, 60000) i;
CREATE INDEX pbs_t ON pbs USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops) WITH (point_tid_columns = 'k');
INSERT INTO pbs SELECT i, i % 7 FROM generate_series(60001, 90000) i;
SELECT sift.col_promote('pbs_t'::regclass) >= 0 AS promoted;
CREATE FUNCTION pbs_check() RETURNS TABLE (segments bool, all_rows_listed bool, ascending bool) LANGUAGE sql AS $$
  SELECT (SELECT count(*) > 1 FROM sift.col_segments('pbs_t') WHERE kind = 'immutable'),
         (SELECT count(*) FROM sift._test_col_tids('pbs_t', '0:in:3|5', 24)) = (SELECT count(*) FROM pbs WHERE k IN (3, 5)),
         (SELECT coalesce(bool_and(tid < next), true) FROM (SELECT tid, lead(tid) OVER (ORDER BY ord) AS next FROM (SELECT row_number() OVER () AS ord, tid FROM sift._test_col_tids('pbs_t', '0:in:3|5', 24)) s) q WHERE next IS NOT NULL)
$$;
SELECT * FROM pbs_check();
DELETE FROM pbs WHERE id % 3 = 0;
VACUUM pbs;
INSERT INTO pbs SELECT i, i % 7 FROM generate_series(90001, 110000) i;
SELECT sift.col_promote('pbs_t'::regclass) >= 0 AS promoted;
SELECT * FROM pbs_check();
DROP FUNCTION pbs_check();
DROP TABLE pbs;
-- 85,000 rows are more than a lookup takes (65,536): the units answer
SELECT p.units > 0 AS units_answer, p.rows = f.rows AS same_rows FROM sift._test_col_counters('pb_t', '0:in:1|2|3', 24) p, sift._test_col_counters('pb_t', '0:in:1|2|3', 0) f;
DROP TABLE pb;

-- the bitmap path of a lookup that another relation keys (a join on a column with a directory of addresses) needs that relation, as the index path under it does:
-- a merge join over the partitions read the key of the outer relation from a plan that did not wait for it
CREATE TABLE pj_i (id int PRIMARY KEY, kind text);
INSERT INTO pj_i SELECT i, 'k' || i % 5 FROM generate_series(0, 599) i;
CREATE TABLE pj (id int, parent_id int, kind text, amount int) PARTITION BY HASH (id);
CREATE TABLE pj_0 PARTITION OF pj FOR VALUES WITH (MODULUS 4, REMAINDER 0);
CREATE TABLE pj_1 PARTITION OF pj FOR VALUES WITH (MODULUS 4, REMAINDER 1);
CREATE TABLE pj_2 PARTITION OF pj FOR VALUES WITH (MODULUS 4, REMAINDER 2);
CREATE TABLE pj_3 PARTITION OF pj FOR VALUES WITH (MODULUS 4, REMAINDER 3);
INSERT INTO pj SELECT i, (i * 7) % 600, 'k' || i % 5, i % 1000 FROM generate_series(1, 1500) i;
SET client_min_messages = warning;
CREATE INDEX pj_pt ON pj USING sift_col (id sift.col_int4_ops, parent_id sift.col_int4_ops, kind sift.col_text_ops, amount sift.col_int4_ops) WITH (count_columns = 'off', point_tid_columns = 'id, parent_id, kind');
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) pj, pj_i;
BEGIN;
SET LOCAL seq_page_cost = 1000; SET LOCAL random_page_cost = 1000; SET LOCAL max_parallel_workers_per_gather = 0; SET LOCAL sift.enable_agg = off;
SELECT count(DISTINCT e.parent_id) FROM pj e WHERE EXISTS (SELECT 1 FROM pj_i i WHERE i.id = e.parent_id AND i.kind = 'k4') AND e.id % 16 = 0;
COMMIT;
DROP TABLE pj, pj_i;

-- not a leaf that can be listed, and a bad list
CREATE TABLE pe (id int, f float8, d date);
CREATE INDEX pe_f ON pe USING sift_col (f sift.col_float8_ops, id sift.col_int4_ops) WITH (point_tid_columns = 'f');
CREATE INDEX pe_x ON pe USING sift_col (id sift.col_int4_ops) WITH (point_tid_columns = 'nope');
ALTER INDEX pt_r SET (point_tid_columns = 'k,');
DROP TABLE pe;

DROP TABLE pt CASCADE;
DROP FUNCTION pt_cover(regclass, text, text), pt_idx(text), pt_bmp(text), pt_seq(text);
DROP TABLE pt_q, pt_w;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
