-- The partitioned manifest of format major 2: a copy on write tree of one, two and three levels, range lookups that read the pages of their range only, splits at the tail
-- of a partition and the merge of neighbours.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ' ' || SQLERRM;
END $$;

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');
SELECT sift._test_f2_add_part('t_idx', 1, NULL, 100, 0, 1) > 0 AS added;

-- A split makes an empty partition above the key; the key must be inside the partition and above every row of its parts
SELECT sift._test_f2_split('t_idx', 1, NULL, 1000);
SELECT regexp_replace(expect('SELECT sift._test_f2_split(''t_idx'', 1, NULL, 50)'), 'partition [0-9]+', 'partition N');
SELECT regexp_replace(expect('SELECT sift._test_f2_split(''t_idx'', 2, 1000, 1000)'), 'partition [0-9]+', 'partition N');
SELECT part_id, key_lo, key_hi, nparts, total_rows FROM sift._test_f2_partitions('t_idx');

-- The empty one merges back
SELECT sift._test_f2_merge('t_idx', 1, NULL, 2, 1000);
SELECT part_id, key_lo, key_hi, nparts, total_rows FROM sift._test_f2_partitions('t_idx');
SELECT sift._test_f2_split('t_idx', 1, NULL, 1000) > 1 AS split_again;

-- Level 1: more than one page of partitions
SELECT part_id AS tail_id FROM sift._test_f2_partitions('t_idx') WHERE key_lo = 1000 \gset
SELECT sift._test_f2_split_many('t_idx', :tail_id, 1000, 150, 1001);
SELECT partdir_level, npartitions, total_rows FROM sift._test_f2_meta('t_idx');
SELECT sift._test_f2_check('t_idx');
SELECT partitions, pages <= 3 AS few_pages FROM sift._test_f2_range('t_idx', 5000, 9000);
SELECT partitions, pages FROM sift._test_f2_range('t_idx', 1, 2);
SELECT partitions, pages > partitions / 100 AS whole_tree FROM sift._test_f2_range('t_idx', NULL, NULL);

-- Level 2: more partitions than a level of 1 holds
SELECT part_id AS last_id, key_lo AS last_lo FROM sift._test_f2_partitions('t_idx') WHERE key_hi IS NULL \gset
SELECT sift._test_f2_split_many('t_idx', :last_id, :last_lo, 4400, 200000);
SELECT partdir_level, npartitions FROM sift._test_f2_meta('t_idx');
SELECT sift._test_f2_check('t_idx');
SELECT partitions, pages <= 4 AS few_pages FROM sift._test_f2_range('t_idx', 400000000, 401000000);
SELECT count(*) AS listed, count(*) FILTER (WHERE key_lo IS NULL) AS first_open, count(*) FILTER (WHERE key_hi IS NULL) AS last_open FROM sift._test_f2_partitions('t_idx');
SELECT partdir_level, npartitions, total_rows FROM sift._test_f2_meta('t_idx');

-- Neighbours that hold parts merge into one partition with the parts of both, while they hold less than 48MB together
CREATE TABLE u (v int);
CREATE INDEX u_idx ON u USING sift_store_test (v);
SELECT sift._test_f2_init('u_idx');
SELECT sift._test_f2_add_part('u_idx', 1, NULL, 100, 0, 1) > 0 AS lo_part;
SELECT sift._test_f2_split('u_idx', 1, NULL, 1000) AS hi_id \gset
SELECT sift._test_f2_add_part('u_idx', :hi_id, 1000, 50, 1000, 101) > 0 AS hi_part;
SELECT sift._test_f2_merge('u_idx', 1, NULL, :hi_id, 1000);
SELECT part_id, key_lo, key_hi, nparts, total_rows FROM sift._test_f2_partitions('u_idx');
SELECT count(*) AS parts, sum(nrows) AS rows FROM sift._test_f2_parts('u_idx', 1, NULL);
SELECT partdir_level, npartitions, total_rows FROM sift._test_f2_meta('u_idx');
SELECT sift._test_f2_check('u_idx');

DROP TABLE u;
DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
