-- Unified granules of format major 2: up to 8192 rows, the _tid as chunk 0, a validity bitmap for the nullable columns, and one encoding for each kind of column chunk.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');

-- Mode 0: every column valid; 20000 rows are three granules (8192, 8192, 3616)
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 20000, 1000, 1, 0, 0, 0) > 0 AS added;
SELECT g AS gen FROM (SELECT min(part_gen) AS g FROM sift._test_f2_parts('t_idx', 1, NULL)) q \gset
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen, 1000, 1, 0);

-- Mode 1: a column of nothing but nulls and a constant one; mode 2: nulls spread over two columns (a validity bitmap)
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 300, 50000, 900000, 0, 0, 2) > 0 AS added;
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 300, 60000, 910000, 0, 0, 1) > 0 AS added;
SELECT max(part_gen) AS gen2 FROM sift._test_f2_parts('t_idx', 1, NULL) WHERE key_lo = 50000 \gset
SELECT max(part_gen) AS gen3 FROM sift._test_f2_parts('t_idx', 1, NULL) WHERE key_lo = 60000 \gset
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen2, 50000, 900000, 2);
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen3, 60000, 910000, 1);

-- Sort order reversed on insert is the part's order all the same
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 1000, 70000, 2000000, 0, 0, 0, false, true) > 0 AS added;
SELECT max(part_gen) AS gen4 FROM sift._test_f2_parts('t_idx', 1, NULL) WHERE key_lo = 70000 \gset
SELECT sift._test_f2_dump('t_idx', 1, NULL, :gen4, 70000, 2000000, 0);
SELECT part_gen > 0 AS ok, level, nrows, key_lo, key_hi, has_xid FROM sift._test_f2_parts('t_idx', 1, NULL) ORDER BY key_lo;
SELECT sift._test_f2_check('t_idx');

-- The time strides of 1 to 4 granules (an even and an odd count) read back exactly
SELECT n, sift._test_f2_tstride('t_idx', n * 8192 - (n % 3) * 100) FROM generate_series(1, 4) n;

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
