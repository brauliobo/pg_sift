-- The slot count of a point directory: ceil(keys * 4 / 3) slots of 8 bytes, a key starts at the position its hash scales to, not a power of
-- two with a mask. Every key of every size is found and an absent one lists no address, a segment of the older layout (sift.col_format_minor 14, tables of a power of two) is read
-- as before, and a merge of both writes the new one.
CREATE EXTENSION pg_sift_test CASCADE;
-- the tables of slots (the packed table of column format minor 35 is the next test, col_point_packed)
SET sift.test_col_point_unpacked = on;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET default_statistics_target = 10000;
SET client_min_messages = warning;

-- one table of n keys per call: the bytes of the directory against the slots it should have, every key found (a sample of 400 for a large table), 400 absent keys listing no more than the
-- chance of a 16 bit fingerprint allows
CREATE FUNCTION pe_probe(n int, minor int) RETURNS TABLE (keys int, format int, slots_ok bool, pow2 bool, all_found bool, absent_ok bool) LANGUAGE plpgsql AS $$
DECLARE slots int; b bigint; step int := greatest(n / 400, 1); lost bigint; extra bigint;
BEGIN
	EXECUTE 'DROP TABLE IF EXISTS pe_t';
	EXECUTE format('CREATE TABLE pe_t AS SELECT g AS id, g * 7919 AS k FROM generate_series(1, %s) g', n);
	PERFORM set_config('sift.col_format_minor', minor::text, true);
	EXECUTE 'CREATE INDEX pe_i ON pe_t USING sift_col (k) WITH (key_cut = ''off'', point_tid_columns = ''k'')';
	PERFORM set_config('sift.col_format_minor', '15', true);
	slots := CASE WHEN minor >= 33 OR minor = 15 THEN greatest(16, ceil(n * 4 / 3.0)::int) ELSE greatest(16, (2 ^ ceil(log(2, n * 4 / 3.0)))::int) END;
	SELECT sum(s.bytes) INTO b FROM sift.col_sections('pe_i') s WHERE s.section = 'dict_other';
	SELECT count(*) INTO lost FROM generate_series(1, n, step) g
		WHERE NOT EXISTS (SELECT 1 FROM sift._test_col_tids('pe_i', '0:eq:' || g * 7919, 24) t JOIN pe_t ON pe_t.ctid = t.tid WHERE pe_t.id = g);
	SELECT count(*) INTO extra FROM generate_series(1, 400) g, sift._test_col_tids('pe_i', '0:eq:' || (-g), 24);
	keys := n;
	SELECT least(min(c.format), 15) INTO format FROM sift.col_segments('pe_i') c WHERE c.kind = 'immutable';
	slots_ok := b BETWEEN slots * 8 AND slots * 8 + 128;
	pow2 := (slots & (slots - 1)) = 0;
	all_found := lost = 0;
	absent_ok := extra < 20;
	EXECUTE 'DROP INDEX pe_i';
	RETURN NEXT;
END $$;

-- the exact count: 16 slots up to 12 keys, then four thirds of the keys, whatever the sizes between two powers of two
SELECT * FROM pe_probe(1, 15);
SELECT * FROM pe_probe(12, 15);
SELECT * FROM pe_probe(13, 15);
SELECT * FROM pe_probe(100, 15);
SELECT * FROM pe_probe(1000, 15);
SELECT * FROM pe_probe(12288, 15);
SELECT * FROM pe_probe(12289, 15);
SELECT * FROM pe_probe(30000, 15);
-- the older layout under the setting 14: a power of two, and the same answers
SELECT * FROM pe_probe(13, 14);
SELECT * FROM pe_probe(1000, 14);
SELECT * FROM pe_probe(12289, 14);

-- a merge of segments of both layouts, with duplicate keys (lists) and NULLs: the answers are the heap's, and the merged segment is the new layout
CREATE TABLE pe_m (id int, k int, s text);
INSERT INTO pe_m SELECT g, CASE WHEN g % 50 = 0 THEN NULL ELSE (g * 7) % 3000 END, 's' || (g * 13) % 2500 FROM generate_series(1, 12000) g;
SET sift.col_format_minor = 14;
CREATE INDEX pe_mi ON pe_m USING sift_col (k, s) WITH (key_cut = 'off', point_tid_columns = 'k, s', count_columns = 'off');
RESET sift.col_format_minor;
INSERT INTO pe_m SELECT g, (g * 7) % 3000, 's' || (g * 13) % 2500 FROM generate_series(12001, 24000) g;
SELECT sift.col_promote('pe_mi'::regclass) >= 0 AS promoted;
SELECT least(format, 15) AS format, count(*) AS segments FROM sift.col_segments('pe_mi') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
CREATE FUNCTION pe_idx(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pe_m WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pe_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM pe_m WHERE %s', w) INTO r;
	RETURN r;
END $$;
CREATE TABLE pe_w (w text);
INSERT INTO pe_w VALUES ('k = 7'), ('k = 2993'), ('k = 3001'), ('k = 0'), ('k IN (1, 2, 3, 700, 2999, 99999)'), ('k IS NULL'), ('s = ''s13'''), ('s = ''s2499'''), ('s = ''absent'''),
	('s IN (''s1'', ''s2'', ''s3'', ''none'')'), ('k = 21 AND s = ''s91'''), ('k = 21 OR s = ''s91''');
SELECT left(w, 36) AS w, pe_idx(w) = pe_seq(w) AS same_rows, cardinality(pe_seq(w)) AS matches FROM pe_w ORDER BY w;
SELECT sift.col_verify('pe_mi', true);
SELECT sift.col_merge('pe_mi'::regclass, true) >= 0 AS merged;
SELECT least(format, 15) AS format, count(*) AS segments FROM sift.col_segments('pe_mi') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT left(w, 36) AS w, pe_idx(w) = pe_seq(w) AS same_rows FROM pe_w ORDER BY w;
SELECT sift.col_verify('pe_mi', true);
-- the older layout is what the setting 14 writes again, by a rebuild
SET sift.col_format_minor = 14;
REINDEX INDEX pe_mi;
RESET sift.col_format_minor;
SELECT least(format, 15) AS format, count(*) AS segments FROM sift.col_segments('pe_mi') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT left(w, 36) AS w, pe_idx(w) = pe_seq(w) AS same_rows FROM pe_w ORDER BY w;
SELECT sift.col_verify('pe_mi', true);

DROP FUNCTION pe_idx(text), pe_seq(text), pe_probe(int, int);
DROP TABLE pe_w, pe_m;
DROP TABLE IF EXISTS pe_t;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
