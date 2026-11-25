-- The limits format major 2 lifts: more than 128 leaves (jsonb paths) in one index and key_order of up to SIFT_KEY_ORDER_MAX columns. Every query runs through the index and again over the heap alone.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

-- 300 jsonb paths and the key: 301 leaves, 128 for format major 1
CREATE TABLE wide (id int, doc jsonb);
INSERT INTO wide SELECT i, (SELECT jsonb_object_agg('k' || j, (i * 7 + j) % 1000) FROM generate_series(0, 299) j) FROM generate_series(1, 400) i;
DO $$ BEGIN
	EXECUTE format('CREATE INDEX wide_idx ON wide USING sift_col (id, doc) WITH (key_order = ''id'', paths = %L)',
				   (SELECT string_agg('doc.k' || j || ' int', ', ' ORDER BY j) FROM generate_series(0, 299) j));
END $$;
SELECT sift._test_f2_check_keys('wide_idx');
SELECT sift_helpers.f2_same('SELECT id FROM wide WHERE (doc->>''k0'')::int = 21') AS k0,
       sift_helpers.f2_same('SELECT id FROM wide WHERE (doc->>''k150'')::int BETWEEN 100 AND 130') AS k150,
       sift_helpers.f2_same('SELECT id FROM wide WHERE (doc->>''k299'')::int = 369') AS k299,
       sift_helpers.f2_same('SELECT id FROM wide WHERE id = 77 AND (doc->>''k128'')::int = 999') AS k128,
       sift_helpers.f2_same('SELECT id FROM wide WHERE (doc->>''k200'')::int = 3 OR id = 5') AS either;
SELECT count(*) > 0 AS rows_found FROM wide WHERE (doc->>'k299')::int = 369;

-- the same number of paths is refused by format major 1
SET sift.test_create_format1 = on;
DO $$ BEGIN
	EXECUTE format('CREATE INDEX wide_v1 ON wide USING sift_col (id, doc) WITH (paths = %L)',
				   (SELECT string_agg('doc.k' || j || ' int', ', ' ORDER BY j) FROM generate_series(0, 299) j));
END $$;
SET sift.test_create_format1 = off;

-- a key_order of five columns, the first three of which repeat: the rows of a key stay sorted by the rest
CREATE TABLE ko (a int, b int, c int, d int, e int, v int);
INSERT INTO ko SELECT i % 3, i % 5, i % 7, i % 11, i % 13, i FROM generate_series(1, 30000) i;
CREATE INDEX ko_idx ON ko USING sift_col (a, b, c, d, e, v) WITH (key_order = 'a, b, c, d, e');
SELECT sift._test_f2_check_keys('ko_idx');
SELECT sift_helpers.f2_same('SELECT v FROM ko WHERE a = 2') AS lead,
       sift_helpers.f2_same('SELECT v FROM ko WHERE a = 1 AND b = 3 AND c = 4') AS three,
       sift_helpers.f2_same('SELECT v FROM ko WHERE d = 5 AND e = 6') AS not_lead,
       sift_helpers.f2_same('SELECT v FROM ko WHERE a BETWEEN 1 AND 2 AND e = 0') AS range_lead;
INSERT INTO ko SELECT i % 3, i % 5, i % 7, i % 11, i % 13, 100000 + i FROM generate_series(1, 200) i;
SELECT sift_helpers.f2_same('SELECT v FROM ko WHERE a = 0 AND b = 0') AS after_insert;
DELETE FROM ko WHERE v % 17 = 0;
VACUUM ko;
SELECT sift._test_f2_check_keys('ko_idx');
SELECT sift_helpers.f2_same('SELECT v FROM ko WHERE a = 1') AS after_delete;

-- eight columns
CREATE TABLE ko8 (c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int);
INSERT INTO ko8 SELECT i % 2, i % 3, i % 4, i % 5, i % 6, i % 7, i % 8, i FROM generate_series(1, 5000) i;
CREATE INDEX ko8_idx ON ko8 USING sift_col (c1, c2, c3, c4, c5, c6, c7, c8) WITH (key_order = 'c1, c2, c3, c4, c5, c6, c7, c8');
SELECT sift._test_f2_check_keys('ko8_idx');
SELECT sift_helpers.f2_same('SELECT c8 FROM ko8 WHERE c1 = 1 AND c2 = 2') AS eight;

-- nine columns, and more than two columns in format major 1, are refused
CREATE INDEX ko9 ON ko8 USING sift_col (c1, c2, c3, c4, c5, c6, c7, c8) WITH (key_order = 'c1, c2, c3, c4, c5, c6, c7, c8, c1');
SET sift.test_create_format1 = on;
CREATE INDEX ko_v1 ON ko USING sift_col (a, b, c, v) WITH (key_order = 'a, b, c');
SET sift.test_create_format1 = off;

DROP TABLE wide, ko, ko8;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
