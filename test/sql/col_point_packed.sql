-- The packed table of a point directory: the entries of a leaf by bucket, each the remainder of its key and the code of its row or heap address,
-- instead of 8-byte slots. Every key of every shape is found with all its rows and an absent one lists almost none, a table that would be larger than the slots (a key in more than
-- 1,024 rows) stays slots, the older layouts are read and merged with it, and sift.col_verify accepts all of it.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET client_min_messages = warning;

-- one table of n rows of the shape per call, one index with the directory of the option (point_tid_columns: addresses, or point_columns: rows of the segment), and what the directory holds:
-- the bytes a row of its block, the layout (packed: the segment is minor 35, written before the root: minor 37 is col_point_root's), the keys of a sample all found with every row of the heap, 300 absent keys listing no more than the chance of a 16 bit fingerprint allows
CREATE FUNCTION pk_probe(shape text, n int, opt text, unpacked bool DEFAULT false) RETURNS TABLE (shape_ text, rows int, bytes_a_row text, packed bool, all_found bool, absent_ok bool, verified bool) LANGUAGE plpgsql AS $$
DECLARE b bigint; step int := greatest(n / 300, 1); lost bigint; extra bigint; fmt int; ix text := 'pk_i';
BEGIN
	EXECUTE 'DROP TABLE IF EXISTS pk_t';
	EXECUTE format('CREATE TABLE pk_t (id int, k int8, pad text) WITH (fillfactor = 50, autovacuum_enabled = false)');
	EXECUTE format('INSERT INTO pk_t SELECT g, %s, repeat(''x'', 20) FROM generate_series(1, %s) g', CASE shape
		WHEN 'dense' THEN '(row_number() OVER (ORDER BY hashint8extended(g, 3)))::int8'
		WHEN 'sparse' THEN 'hashint8extended(g, 5) & 4611686018427387903'
		WHEN 'dupes' THEN '(g * 7919) % ' || (n / 3)
		WHEN 'heavy' THEN 'CASE WHEN g % 3 = 0 THEN 7 ELSE g END'
		WHEN 'extreme' THEN 'CASE g % 4 WHEN 0 THEN 9223372036854775807 - g % 50 WHEN 1 THEN -9223372036854775807 + g % 50 ELSE g END'
		WHEN 'down' THEN '-g * 3 - 1000000' END, n);
	EXECUTE 'ANALYZE pk_t';
	PERFORM set_config('sift.test_col_point_unpacked', unpacked::text, true);
	PERFORM set_config('sift.test_col_point_unrooted', 'on', true);
	EXECUTE format('CREATE INDEX pk_i ON pk_t USING sift_col (k, id) WITH (key_cut = ''off'', %s)', opt);
	PERFORM set_config('sift.test_col_point_unpacked', 'off', true);
	PERFORM set_config('sift.test_col_point_unrooted', 'off', true);
	SELECT sum(s.bytes) INTO b FROM sift.col_sections('pk_i') s WHERE s.section = 'dict_other';
	SELECT min(c.format) INTO fmt FROM sift.col_segments('pk_i') c WHERE c.kind = 'immutable';
	SELECT count(*) INTO lost FROM generate_series(1, n, step) g
		WHERE EXISTS (SELECT 1 FROM pk_t h WHERE h.id = g AND NOT EXISTS (SELECT 1 FROM sift._test_col_tids('pk_i', '0:eq:' || h.k, 24) t WHERE t.tid = h.ctid));
	SELECT count(*) INTO extra FROM generate_series(1, 300) g, sift._test_col_tids('pk_i', '0:eq:' || (4000000000000000000 + g), 24);
	shape_ := shape;
	rows := n;
	bytes_a_row := CASE WHEN n < 1000 THEN 'small' WHEN b * 1.0 / n < 4.5 THEN 'under 4.5' WHEN b * 1.0 / n < 6 THEN 'under 6' WHEN b * 1.0 / n < 9 THEN 'under 9' ELSE 'over 9' END;
	packed := fmt >= 35;
	all_found := lost = 0;
	absent_ok := extra < 20;
	verified := (SELECT count(*) FROM sift.col_verify('pk_i', true)) >= 0;
	EXECUTE 'DROP INDEX pk_i';
	RETURN NEXT;
END $$;

-- addresses: a dense range of keys is exact (no 16 bit fingerprint), random 62 bit keys are hashed, and a key in several rows lists each
SELECT * FROM pk_probe('dense', 20000, 'point_tid_columns = ''k''');
SELECT * FROM pk_probe('sparse', 20000, 'point_tid_columns = ''k''');
SELECT * FROM pk_probe('dupes', 30000, 'point_tid_columns = ''k''');
SELECT * FROM pk_probe('down', 20000, 'point_tid_columns = ''k''');
SELECT * FROM pk_probe('extreme', 20000, 'point_tid_columns = ''k''');
-- a key in a third of the rows is not packed (a bucket of 10,000 entries): the slots and a list, as before
SELECT * FROM pk_probe('heavy', 30000, 'point_tid_columns = ''k''');
-- rows of the segment, not addresses
SELECT * FROM pk_probe('dense', 20000, 'point_columns = ''k''');
SELECT * FROM pk_probe('sparse', 20000, 'point_columns = ''k''');
SELECT * FROM pk_probe('dupes', 30000, 'point_columns = ''k''');
-- the slots, for the same shapes
SELECT * FROM pk_probe('dense', 20000, 'point_tid_columns = ''k''', true);
SELECT * FROM pk_probe('sparse', 20000, 'point_columns = ''k''', true);
-- few rows, and one
SELECT * FROM pk_probe('dense', 40, 'point_tid_columns = ''k''');
SELECT * FROM pk_probe('sparse', 3, 'point_tid_columns = ''k''');
SELECT * FROM pk_probe('dense', 1, 'point_tid_columns = ''k''');

-- segments of every layout in one index, then merged: the slots of the older minors, the packed table, the packed table with its header first (minor 35) and last (minor 37), and the answers are the heap's
CREATE TABLE pk_m (id int, k int8, s text);
INSERT INTO pk_m SELECT g, (g * 7919) % 5000, 's' || (g * 13) % 2500 FROM generate_series(1, 12000) g;
SET sift.col_format_minor = 14;
CREATE INDEX pk_mi ON pk_m USING sift_col (k, s) WITH (key_cut = 'off', point_tid_columns = 'k, s', count_columns = 'off');
RESET sift.col_format_minor;
SET sift.test_col_point_unpacked = on;
SET sift.test_col_point_unrooted = on;
INSERT INTO pk_m SELECT g, (g * 7919) % 5000, 's' || (g * 13) % 2500 FROM generate_series(12001, 24000) g;
SELECT sift.col_promote('pk_mi'::regclass) >= 0 AS promoted;
RESET sift.test_col_point_unpacked;
INSERT INTO pk_m SELECT g, (g * 7919) % 5000 + 100000, 's' || (g * 13) % 2500 FROM generate_series(24001, 36000) g;
SELECT sift.col_promote('pk_mi'::regclass) >= 0 AS promoted;
RESET sift.test_col_point_unrooted;
INSERT INTO pk_m SELECT g, (g * 7919) % 5000 + 200000, 's' || (g * 13) % 2500 FROM generate_series(36001, 48000) g;
SELECT sift.col_promote('pk_mi'::regclass) >= 0 AS promoted;
SELECT least(format, 15) AS format, format >= 35 AS packed, format >= 37 AS rooted, count(*) AS segments FROM sift.col_segments('pk_mi') WHERE kind = 'immutable' GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
CREATE FUNCTION pk_idx(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pk_m WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pk_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pk_m WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE TABLE pk_w (w text);
INSERT INTO pk_w VALUES ('k = 7'), ('k = 4999'), ('k = 5001'), ('k = 100007'), ('k = 0'), ('k IN (1, 2, 3, 700, 2999, 99999, 100000)'), ('s = ''s13'''), ('s = ''s2499'''), ('s = ''absent'''),
	('s IN (''s1'', ''s2'', ''s3'', ''none'')'), ('k = 21 AND s = ''s91'''), ('k = 21 OR s = ''s91''');
SELECT left(w, 36) AS w, pk_idx(w) = pk_seq(w) AS same_rows, cardinality(pk_seq(w)) AS matches FROM pk_w ORDER BY w;
SELECT sift.col_verify('pk_mi', true);
SELECT sift.col_merge('pk_mi'::regclass, true) >= 0 AS merged;
SELECT least(format, 15) AS format, format >= 35 AS packed, format >= 37 AS rooted, count(*) AS segments FROM sift.col_segments('pk_mi') WHERE kind = 'immutable' GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
SELECT left(w, 36) AS w, pk_idx(w) = pk_seq(w) AS same_rows FROM pk_w ORDER BY w;
SELECT sift.col_verify('pk_mi', true);
-- the older layout is what the setting 14 writes again, by a rebuild
SET sift.col_format_minor = 14;
REINDEX INDEX pk_mi;
RESET sift.col_format_minor;
SELECT least(format, 15) AS format, format >= 35 AS packed, format >= 37 AS rooted, count(*) AS segments FROM sift.col_segments('pk_mi') WHERE kind = 'immutable' GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
SELECT left(w, 36) AS w, pk_idx(w) = pk_seq(w) AS same_rows FROM pk_w ORDER BY w;
SELECT sift.col_verify('pk_mi', true);

-- a bit flipped in the frame (the header, the records, the parameters of the tables, the footer) is refused when the segment is opened, one in a table when sift.col_verify compares it with the heap
CREATE TABLE pk_d AS SELECT g AS id, g * 7 AS k, hashint8extended(g, 5) & 4611686018427387903 AS h FROM generate_series(1, 3000) g;
CREATE INDEX pk_one ON pk_d USING sift_col (k, h) WITH (key_cut = 'off', point_tid_columns = 'k, h');
SELECT least(min(format), 15) AS format, min(format) >= 35 AS packed FROM sift.col_segments('pk_one') WHERE kind = 'immutable';
SELECT runs > 0 AS has_sections, damaged > 100 AS many, refused = damaged AS all_refused, unnoticed FROM sift._test_col_point_file_damage('pk_one', 1000000000);
SELECT runs > 0 AS has_sections, damaged > 100 AS many, refused > 0 AS some_refused FROM sift._test_col_point_file_damage('pk_one', 97);
DROP INDEX pk_one;
DROP TABLE pk_d;

DROP FUNCTION pk_idx(text), pk_seq(text), pk_probe(text, int, text, bool);
DROP TABLE pk_w, pk_m;
DROP TABLE IF EXISTS pk_t;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
