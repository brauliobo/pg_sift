-- The point directory of a leaf in a segment (column format minor 14, docs/COL.md 2.17, the option point_columns): the rows of an equality or a
-- short IN list of the access method found with a probe, the heap checking the condition again. The answers are the heap's, over NULLs, duplicates,
-- negative and large keys, text and uuid, DML, the write buffer, promotion and a merge; a leaf that cannot be listed (a key in very many rows, an
-- inline string) gets no table, and an index written without the option is read as before.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;

CREATE TABLE pd (id int, k int, s text, u uuid, v bigint, pad text DEFAULT repeat('x', 400));
ALTER TABLE pd ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pd (id, k, s, u, v)
SELECT i,
	   CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7919) % 9000 END,
	   CASE WHEN i % 47 = 0 THEN NULL WHEN i % 41 = 0 THEN '' ELSE 's' || (i * 31) % 6000 END,
	   CASE WHEN i % 59 = 0 THEN NULL ELSE md5((i % 5000)::text)::uuid END,
	   (i::bigint * 1000003) % 500000 - 250000
FROM generate_series(1, 30000) i;

-- the rows the source gives with the directory: the heap's
CREATE FUNCTION pd_cmp(idx regclass, preds text, heapwhere text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds, 8);
	EXECUTE format('SELECT count(*) FROM pd WHERE %s', heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L, 8) EXCEPT SELECT ctid FROM pd WHERE %s) z', idx, preds, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM pd WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L, 8)) z', heapwhere, idx, preds) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN 'ok' ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

-- the answer of the access method by an index scan, by a bitmap scan, and of the heap
CREATE FUNCTION pd_idx(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pd WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pd_bmp(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_indexscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pd WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pd_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pd WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pd_plan(w text) RETURNS text LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT id FROM pd WHERE %s', w) LOOP
		IF l ~ 'Scan using' THEN RETURN regexp_replace(trim(l), ' on pd.*', ''); END IF;
	END LOOP;
	RETURN 'no index scan';
END $$;

-- leaves of the index pd_all: 0 is k, 1 is s, 2 is u, 3 is v, 4 is id
CREATE TABLE pd_q (n serial, preds text, heapwhere text);
INSERT INTO pd_q (preds, heapwhere) VALUES
 ('0:eq:4321', 'k = 4321'),
 ('0:eq:0', 'k = 0'),
 ('0:eq:99999', 'k = 99999'),
 ('0:eq:-1', 'k = -1'),
 ('0:in:1|2|3|4000|8999', 'k IN (1, 2, 3, 4000, 8999)'),
 ('0:in:5|5|5', 'k IN (5, 5, 5)'),
 ('0:ge:100;0:le:103', 'k BETWEEN 100 AND 103'),
 ('0:ge:100;0:le:300', 'k BETWEEN 100 AND 300'),
 ('0:lt:5', 'k < 5'),
 ('0:null:', 'k IS NULL'),
 ('0:notnull:', 'k IS NOT NULL'),
 ('0:ne:4321', 'k <> 4321'),
 ('0:eq:4321;3:eq:' || ((4321::bigint * 1000003) % 500000 - 250000), 'k = 4321 AND v = ' || ((4321::bigint * 1000003) % 500000 - 250000)),
 ('0:eq:4321;3:eq:7', 'k = 4321 AND v = 7'),
 ('1:eq:s77', 's = ''s77'''),
 ('1:eq:', 's = '''''),
 ('1:eq:nothing', 's = ''nothing'''),
 ('1:in:s1|s2|s3|none', 's IN (''s1'', ''s2'', ''s3'', ''none'')'),
 ('1:prefix:s5999', 'starts_with(s, ''s5999'')'),
 ('1:prefix:s59', 'starts_with(s, ''s59'')'),
 ('1:prefix:s5', 'starts_with(s, ''s5'')'),
 ('2:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 'u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b'''),
 ('2:in:c4ca4238-a0b9-2382-0dcc-509a6f75849b|c81e728d-9d4c-2f63-6f06-7f89cc14862c', 'u IN (''c4ca4238-a0b9-2382-0dcc-509a6f75849b'', ''c81e728d-9d4c-2f63-6f06-7f89cc14862c'')'),
 ('2:eq:00000000-0000-0000-0000-000000000001', 'u = ''00000000-0000-0000-0000-000000000001'''),
 ('3:eq:-250000', 'v = -250000'),
 ('3:eq:249999', 'v = 249999'),
 ('3:in:-250000|0|249999', 'v IN (-250000, 0, 249999)'),
 ('4:eq:777', 'id = 777'),
 ('1:eq:s77;2:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 's = ''s77'' AND u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b''');

-- the same over the index pd_k: leaf 0 is k, 1 is v, 2 is id
CREATE TABLE pd_qk (n serial, preds text, heapwhere text);
INSERT INTO pd_qk (preds, heapwhere) VALUES
 ('0:eq:4321', 'k = 4321'),
 ('0:eq:99999', 'k = 99999'),
 ('0:in:1|2|3|4000|8999', 'k IN (1, 2, 3, 4000, 8999)'),
 ('0:ge:100;0:le:103', 'k BETWEEN 100 AND 103'),
 ('0:ge:100;0:le:300', 'k BETWEEN 100 AND 300'),
 ('0:lt:5', 'k < 5'),
 ('0:null:', 'k IS NULL'),
 ('0:ne:4321', 'k <> 4321'),
 ('0:eq:4321;1:eq:' || ((4321::bigint * 1000003) % 500000 - 250000), 'k = 4321 AND v = ' || ((4321::bigint * 1000003) % 500000 - 250000)),
 ('0:eq:4321;1:eq:7', 'k = 4321 AND v = 7'),
 ('1:eq:-250000', 'v = -250000'),
 ('1:in:-250000|0|249999', 'v IN (-250000, 0, 249999)'),
 ('2:eq:777', 'id = 777');

-- the directory: the segment is minor 14 because of it (no dictionary of 1,024 strings is in the index), verify lists every row under its key, and a
-- point query reads the units that hold its rows only
CREATE INDEX pd_k ON pd USING sift_col (k sift.col_int4_ops, v sift.col_int8_ops, id sift.col_int4_ops) WITH (write_buffer_limit = 256, point_columns = 'k, v');
CREATE INDEX pd_n ON pd USING sift_col (k sift.col_int4_ops, v sift.col_int8_ops, id sift.col_int4_ops) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) pd;
SELECT sift.col_verify('pd_k', true), sift.col_verify('pd_n', true);
SELECT index, count(*) AS segments, min(format) AS format, sum(units) > 2 AS units
FROM (SELECT 'pd_k' AS index, * FROM sift.col_segments('pd_k') UNION ALL SELECT 'pd_n', * FROM sift.col_segments('pd_n')) x
WHERE kind = 'immutable' GROUP BY index ORDER BY index;
SELECT p.units < f.units AS fewer_units, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pd_k', '0:eq:4321', 8) p, sift._test_col_counters('pd_k', '0:eq:4321', 0) f;
SELECT p.units < f.units AS fewer_units, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pd_k', '0:in:4321|99999', 8) p, sift._test_col_counters('pd_k', '0:in:4321|99999', 0) f;
SELECT p.units = f.units AS the_same_units_for_a_range, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pd_k', '0:ge:100;0:le:300', 8) p, sift._test_col_counters('pd_k', '0:ge:100;0:le:300', 0) f;
SELECT p.units = f.units AS the_same_units_for_a_leaf_without, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pd_k', '2:eq:777', 8) p, sift._test_col_counters('pd_k', '2:eq:777', 0) f;
SELECT n, pd_cmp('pd_k', preds, heapwhere), pd_cmp('pd_n', preds, heapwhere) FROM pd_qk ORDER BY n;
DROP INDEX pd_n;

-- text and uuid leaves: the directory lists their codes, and a dictionary of many strings is stored for a lookup (minor 14 as well). The parallel build splits the rows
-- into segments by how its workers take the heap blocks, so the units of a lookup are bounded by the superblocks of the rows and by the rows (every unit read holds one);
-- a build without workers (pd_one) is one segment, and a lookup reads exactly the superblocks that hold its rows
CREATE INDEX pd_all ON pd USING sift_col (k sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_int8_ops, id sift.col_int4_ops)
	WITH (write_buffer_limit = 256, point_columns = 'k, s, u, v');
SELECT sift.col_verify('pd_all', true);
SELECT min(format) AS format FROM sift.col_segments('pd_all') WHERE kind = 'immutable';
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q ORDER BY n;
SELECT p.units BETWEEN sb.n AND p.rows AS units_hold_the_rows_text, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pd_all', '1:eq:s77', 8) p, sift._test_col_counters('pd_all', '1:eq:s77', 0) f,
	 (SELECT count(DISTINCT (ctid::text::point)[0]::int / 256) AS n FROM pd WHERE s = 's77') sb;
SELECT p.units BETWEEN sb.n AND p.rows AS units_hold_the_rows_uuid, p.rows = f.rows AS same_rows
FROM sift._test_col_counters('pd_all', '2:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 8) p, sift._test_col_counters('pd_all', '2:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 0) f,
	 (SELECT count(DISTINCT (ctid::text::point)[0]::int / 256) AS n FROM pd WHERE u = md5('1')::uuid) sb;

SET max_parallel_maintenance_workers = 0;
CREATE INDEX pd_one ON pd USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, k sift.col_int4_ops, id sift.col_int4_ops) WITH (write_buffer_limit = 256, point_columns = 's, u, k');
RESET max_parallel_maintenance_workers;
SELECT sift.col_verify('pd_one', true), (SELECT count(*) FROM sift.col_segments('pd_one') WHERE kind = 'immutable') AS segments;
SELECT p.units = sb.n AS exact_units_text, p.rows = f.rows AS same_rows, p.rows, p.units
FROM sift._test_col_counters('pd_one', '0:eq:s77', 8) p, sift._test_col_counters('pd_one', '0:eq:s77', 0) f,
	 (SELECT count(DISTINCT (ctid::text::point)[0]::int / 256) AS n FROM pd WHERE s = 's77') sb;
SELECT p.units = sb.n AS exact_units_uuid, p.rows = f.rows AS same_rows, p.rows, p.units
FROM sift._test_col_counters('pd_one', '1:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 8) p, sift._test_col_counters('pd_one', '1:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 0) f,
	 (SELECT count(DISTINCT (ctid::text::point)[0]::int / 256) AS n FROM pd WHERE u = md5('1')::uuid) sb;
SELECT p.units = sb.n AS exact_units_empty_string, p.rows = f.rows AS same_rows, p.rows, p.units
FROM sift._test_col_counters('pd_one', '0:eq:', 8) p, sift._test_col_counters('pd_one', '0:eq:', 0) f,
	 (SELECT count(DISTINCT (ctid::text::point)[0]::int / 256) AS n FROM pd WHERE s = '') sb;
-- the point directories (the header, the records of the leaves and the footer, then the tables of slots and the lists) read as themselves with a bit flipped,
-- one damage at a time, through a fresh open and the check of the segment: never outside the section (meaningful under SANITIZE=1). With a stride past
-- the section only the header, the records and the footer are damaged: the ones the check of the segment cannot know (reserved bytes, a fingerprint) are counted
-- as unnoticed; with a stride of 997 a byte in 997 of the tables and lists too (sift.col_verify compares those with the heap)
SELECT runs > 0 AS has_sections, damaged > 100 AS many, refused > 0 AS some_refused, unnoticed FROM sift._test_col_point_file_damage('pd_one', 1000000000);
SELECT runs > 0 AS has_sections, damaged > 100 AS many, refused > 0 AS some_refused FROM sift._test_col_point_file_damage('pd_one', 997);
DROP INDEX pd_one;

-- the access method: the plans, and the rows of an index scan and a bitmap scan are the heap's
CREATE TABLE pd_w (n serial, w text);
INSERT INTO pd_w (w) VALUES
 ('k = 4321'), ('k = 99999'), ('k IN (1, 2, 3, 4000, 8999)'), ('k IS NULL'), ('k = 4321 AND v > -100000'), ('k BETWEEN 100 AND 103'), ('k BETWEEN 100 AND 300'),
 ('s = ''s77'''), ('s IN (''s1'', ''s2'', ''none'')'), ('s = '''''), ('starts_with(s, ''s5999'')'),
 ('u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b'''), ('u IN (''c4ca4238-a0b9-2382-0dcc-509a6f75849b'', ''c81e728d-9d4c-2f63-6f06-7f89cc14862c'')'),
 ('v = -250000'), ('v IN (-250000, 0, 249999)'), ('k = 4321 AND s = ''s77'''), ('k = 4321 OR s = ''s77''');
SELECT n, w, pd_plan(w) FROM pd_w WHERE n IN (1, 3, 8, 12) ORDER BY n;
SELECT n, w, pd_idx(w) = pd_seq(w) AS index_scan, pd_bmp(w) = pd_seq(w) AS bitmap_scan, cardinality(pd_seq(w)) AS matches FROM pd_w ORDER BY n;

-- DML, the write buffer, promotion and a merge of segments with and without a directory
UPDATE pd SET k = 4321, s = 's77', v = 7 WHERE id % 1999 = 0;
DELETE FROM pd WHERE id % 89 = 0;
INSERT INTO pd (id, k, s, u, v) SELECT i, 4321, 's77', 'c4ca4238-a0b9-2382-0dcc-509a6f75849b', 7 FROM generate_series(30001, 30050) i;
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q WHERE n IN (1, 5, 13, 14, 15, 22) ORDER BY n;
SELECT n, w, pd_idx(w) = pd_seq(w) AS index_scan, pd_bmp(w) = pd_seq(w) AS bitmap_scan FROM pd_w WHERE n IN (1, 3, 8, 12, 16) ORDER BY n;
VACUUM pd;
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q ORDER BY n;
SELECT n, w, pd_idx(w) = pd_seq(w) AS index_scan, pd_bmp(w) = pd_seq(w) AS bitmap_scan FROM pd_w ORDER BY n;
SET sift.col_format_minor = 13;
INSERT INTO pd (id, k, s, u, v) SELECT i, i % 9000, 'old' || i % 500, md5(i::text)::uuid, i FROM generate_series(30101, 31100) i;
SELECT sift.col_promote('pd_all'::regclass) >= 0 AS promoted;
RESET sift.col_format_minor;
SELECT kind, format, count(*) AS segments FROM sift.col_segments('pd_all') GROUP BY kind, format ORDER BY kind, format;
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q ORDER BY n;
SELECT n, w, pd_idx(w) = pd_seq(w) AS index_scan, pd_bmp(w) = pd_seq(w) AS bitmap_scan FROM pd_w ORDER BY n;
SELECT sift.col_merge('pd_all'::regclass, true) >= 0 AS merged;
SELECT kind, format, count(*) AS segments FROM sift.col_segments('pd_all') GROUP BY kind, format ORDER BY kind, format;
SELECT sift.col_verify('pd_all', true);
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q ORDER BY n;
SELECT n, w, pd_idx(w) = pd_seq(w) AS index_scan, pd_bmp(w) = pd_seq(w) AS bitmap_scan FROM pd_w ORDER BY n;

-- an index written at the setting 13 has no directory and is read as before; ALTER INDEX applies to the segments written afterwards
SET sift.col_format_minor = 13;
CREATE INDEX pd_13 ON pd USING sift_col (k sift.col_int4_ops, v sift.col_int8_ops, id sift.col_int4_ops) WITH (point_columns = 'k, v');
RESET sift.col_format_minor;
SELECT min(format) < 13 AS no_directory FROM sift.col_segments('pd_13') WHERE kind = 'immutable';
SELECT n, pd_cmp('pd_13', preds, heapwhere) FROM pd_qk ORDER BY n;
SELECT sift.col_verify('pd_13', true);
DROP INDEX pd_13, pd_k;
ALTER INDEX pd_all SET (point_columns = 'k');
REINDEX INDEX pd_all;
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q ORDER BY n;
SELECT sift.col_verify('pd_all', true);
ALTER INDEX pd_all RESET (point_columns);
REINDEX INDEX pd_all;
SELECT n, pd_cmp('pd_all', preds, heapwhere) FROM pd_q ORDER BY n;

-- a string kept inline cannot be listed: the leaf gets no table; a key in very many rows does not take the table of the other keys with it
CREATE TABLE ph (id int, k int, s text, pad text DEFAULT repeat('x', 100));
INSERT INTO ph SELECT i, CASE WHEN i % 10 = 0 THEN i ELSE 1 END, CASE WHEN i = 2500 THEN repeat('long', 3000) ELSE 's' || i % 700 END FROM generate_series(1, 9000) i;
CREATE INDEX ph_k ON ph USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops) WITH (point_columns = 'k');
CREATE INDEX ph_s ON ph USING sift_col (s sift.col_text_ops, id sift.col_int4_ops) WITH (point_columns = 's');
SELECT (SELECT min(format) FROM sift.col_segments('ph_k') WHERE kind = 'immutable') >= 14 AS hot_key_has_a_directory,
	   (SELECT min(format) FROM sift.col_segments('ph_s') WHERE kind = 'immutable') < 14 AS inline_string_has_no_directory;
SELECT sift.col_verify('ph_k', true), sift.col_verify('ph_s', true);
SELECT (SELECT count(*) FROM sift._test_col_tids('ph_k', '0:eq:1', 8)) AS hot_key, (SELECT count(*) FROM ph WHERE k = 1) AS heap_hot_key,
	   (SELECT count(*) FROM sift._test_col_tids('ph_k', '0:eq:30', 8)) AS cold_key, (SELECT count(*) FROM ph WHERE k = 30) AS heap_cold_key,
	   (SELECT count(*) FROM sift._test_col_tids('ph_s', '0:eq:s7', 8)) AS text, (SELECT count(*) FROM ph WHERE s = 's7') AS heap_text;
DROP TABLE ph;

-- a key in more rows than a lookup takes (65,536) is answered by the units, and the other keys of the leaf are still answered by the directory, one unit for one row
CREATE TABLE phb (id int, k int, pad text DEFAULT repeat('x', 20));
INSERT INTO phb SELECT i, CASE WHEN i % 100 = 0 THEN i ELSE 1 END FROM generate_series(1, 70000) i;
CREATE INDEX phb_k ON phb USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops) WITH (point_columns = 'k');
SELECT sift.col_verify('phb_k', true);
SELECT p.units = f.units AS hot_key_read_by_the_units, p.rows = f.rows AS same_rows, p.rows AS rows
FROM sift._test_col_counters('phb_k', '0:eq:1', 8) p, sift._test_col_counters('phb_k', '0:eq:1', 0) f;
SELECT p.units = 1 AS cold_key_one_unit, p.rows = f.rows AS same_rows, p.rows AS rows
FROM sift._test_col_counters('phb_k', '0:eq:3000', 8) p, sift._test_col_counters('phb_k', '0:eq:3000', 0) f;
DROP TABLE phb;

-- keys at the ends of the int8 range (infinity and -infinity of a timestamp are those of an int8): a range up to the largest key lists its keys without running past it
CREATE TABLE px AS
SELECT i AS id, CASE i % 5 WHEN 0 THEN 9223372036854775807 WHEN 1 THEN -9223372036854775807 - 1 ELSE i END AS v,
	   CASE i % 7 WHEN 0 THEN 'infinity'::timestamp WHEN 1 THEN '-infinity' ELSE timestamp '2024-01-01' + i * interval '1 minute' END AS ts
FROM generate_series(1, 3000) i;
CREATE INDEX px_i ON px USING sift_col (id sift.col_int4_ops, v sift.col_int8_ops, ts sift.col_timestamp_ops) WITH (point_columns = 'v, ts');
SELECT sift.col_verify('px_i', true);
SELECT (SELECT count(*) FROM sift._test_col_tids('px_i', '1:eq:9223372036854775807', 8)) AS v_max, (SELECT count(*) FROM px WHERE v = 9223372036854775807) AS heap_v_max,
	   (SELECT count(*) FROM sift._test_col_tids('px_i', '1:eq:-9223372036854775808', 8)) AS v_min, (SELECT count(*) FROM px WHERE v = -9223372036854775807 - 1) AS heap_v_min,
	   (SELECT count(*) FROM sift._test_col_tids('px_i', '1:ge:9223372036854775800;1:le:9223372036854775807', 8)) AS v_top_range, (SELECT count(*) FROM px WHERE v >= 9223372036854775800) AS heap_v_top_range,
	   (SELECT count(*) FROM sift._test_col_tids('px_i', '2:eq:9223372036854775807', 8)) AS infinity, (SELECT count(*) FROM px WHERE ts = 'infinity') AS heap_infinity,
	   (SELECT count(*) FROM sift._test_col_tids('px_i', '2:eq:-9223372036854775808', 8)) AS minus_infinity, (SELECT count(*) FROM px WHERE ts = '-infinity') AS heap_minus_infinity;
DROP TABLE px;

-- the units a lookup reads, over a segment of 65 units (a second word of the keep bitmap holds the last unit): the units pruned are skipped a word at a time, so a key in
-- the first, the last of a word, the first of the next word, in two adjacent ones across the words, in a middle one and in none reads exactly those units, with the
-- directory, with the zones alone, and the rows are the heap's. A page holds one row, so unit n holds the rows 256 * n .. 256 * n + 255
CREATE TABLE pw (id int, k int, pad text) WITH (fillfactor = 10);
INSERT INTO pw SELECT i, CASE i / 256 WHEN 0 THEN 1 WHEN 63 THEN 2 WHEN 64 THEN 3 ELSE 100 + i / 256 END, repeat('x', 600) FROM generate_series(0, 65 * 256 - 1) i;
SET max_parallel_maintenance_workers = 0;
CREATE INDEX pw_k ON pw USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops) WITH (point_columns = 'k');
CREATE INDEX pw_n ON pw USING sift_col (k sift.col_int4_ops, id sift.col_int4_ops);
RESET max_parallel_maintenance_workers;
SELECT units FROM sift.col_segments('pw_k') WHERE kind = 'immutable';
SELECT w.preds, d.units AS dir_units, d.units_pruned AS dir_pruned, z.units AS zone_units, z.units_pruned AS zone_pruned, d.rows = h.n AS dir_rows_ok, z.rows = h.n AS zone_rows_ok
FROM (VALUES ('0:eq:1', ARRAY[1]), ('0:eq:2', ARRAY[2]), ('0:eq:3', ARRAY[3]), ('0:in:1|3', ARRAY[1, 3]), ('0:in:2|3', ARRAY[2, 3]), ('0:in:1|2|3', ARRAY[1, 2, 3]),
			 ('0:eq:117', ARRAY[117]), ('0:in:101|162', ARRAY[101, 162]), ('0:eq:5', ARRAY[5]), ('0:in:5|6', ARRAY[5, 6])) w (preds, keys),
	 LATERAL sift._test_col_counters('pw_k', w.preds, 8) d,
	 LATERAL sift._test_col_counters('pw_n', w.preds, 0) z,
	 LATERAL (SELECT count(*) AS n FROM pw WHERE k = ANY (w.keys)) h
ORDER BY w.preds;
DROP TABLE pw;

-- the option names plain integer, text and uuid columns of the top level (a date is an integer leaf)
CREATE TABLE pe (id int, f float8, j jsonb, d date);
CREATE INDEX pe_f ON pe USING sift_col (f sift.col_float8_ops, id sift.col_int4_ops) WITH (point_columns = 'f');
CREATE INDEX pe_x ON pe USING sift_col (id sift.col_int4_ops) WITH (point_columns = 'nope');
CREATE INDEX pe_s ON pe USING sift_col (id sift.col_int4_ops, d sift.col_date_ops) WITH (point_columns = 'id, d');
DROP TABLE pe;
ALTER INDEX pd_all SET (point_columns = 'k,');
DROP TABLE pd CASCADE;
DROP FUNCTION pd_cmp(regclass, text, text), pd_idx(text), pd_bmp(text), pd_seq(text), pd_plan(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
DROP TABLE pd_q, pd_qk, pd_w;
