-- A directory of more entries than block 1 holds is a chain of pages that every caller reads, edits and merges whole: with block 1 and the
-- leaves cut small, an index of 90 segments of the search method and one of 80 segments of the column method are read, queried, deleted from,
-- vacuumed, merged, shrunk and verified
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET client_min_messages = warning;
SET sift.test_dir_flat_max = 4;
SET sift.test_dir_leaf_max = 8;

CREATE TABLE wide (id int PRIMARY KEY, body text);
CREATE INDEX wide_idx ON wide USING sift (body) WITH (write_buffer_limit = 64, merge_fanin = 32);
CREATE PROCEDURE fill_wide(batches int) LANGUAGE plpgsql AS $$
BEGIN
	FOR b IN 1..batches LOOP
		INSERT INTO wide SELECT g, 'w' || (g % 50) || ' common' FROM generate_series(b * 3, b * 3 + 2) g;
		PERFORM sift.promote('wide_idx');
	END LOOP;
END $$;
CALL fill_wide(90);

SELECT count(*) FILTER (WHERE kind = 'immutable') AS segments, count(*) > 4 AS past_block_1 FROM sift.segments('wide_idx');
SELECT chained, nsegs > 4 AS more_than_block_1 FROM sift._test_dir_info('wide_idx');
SELECT sift._test_dir_check('wide_idx') > 0 AS chain_holds;
SELECT sift.verify('wide_idx', true) IS NOT NULL AS verified;
SELECT count(*) AS hits FROM wide WHERE body @@> 'w7 common';
SELECT (SELECT count(*) FROM wide WHERE body @@> 'w7 common') = (SELECT count(*) FROM wide WHERE id % 50 = 7) AS agrees_with_the_heap;

SELECT sift.merge('wide_idx') > 0 AS merged;
SELECT count(*) FILTER (WHERE kind = 'immutable') < 90 AS merged_down FROM sift.segments('wide_idx');
SELECT count(*) AS hits FROM wide WHERE body @@> 'w7 common';

DELETE FROM wide WHERE id % 3 = 0;
VACUUM wide;
SELECT count(*) AS hits FROM wide WHERE body @@> 'w7 common';
SELECT (SELECT count(*) FROM wide WHERE body @@> 'w7 common') = (SELECT count(*) FROM wide WHERE id % 50 = 7) AS agrees_with_the_heap;
SELECT sift.verify('wide_idx', true) IS NOT NULL AS verified;
SELECT pg_relation_size('wide_idx') / 8192 < 100 AS file_shrunk;
SELECT sift._test_dir_check('wide_idx') > 0 AS chain_holds;

CREATE TABLE cw (id int PRIMARY KEY, k int, v int);
CREATE INDEX cw_idx ON cw USING sift_col (k, v);
CREATE PROCEDURE fill_cw(batches int) LANGUAGE plpgsql AS $$
BEGIN
	FOR b IN 1..batches LOOP
		INSERT INTO cw SELECT g, g % 7, g % 13 FROM generate_series(b * 3, b * 3 + 2) g;
		PERFORM sift.col_promote('cw_idx');
	END LOOP;
END $$;
CALL fill_cw(80);

SELECT count(*) > 4 AS past_block_1 FROM sift.col_segments('cw_idx');
SELECT chained FROM sift._test_dir_info('cw_idx');
SELECT sift.col_verify('cw_idx') IS NOT NULL AS verified;
SELECT sum(v), count(*) FROM cw;
SELECT k, sum(v) FROM cw GROUP BY k ORDER BY k;
SELECT sift.col_merge('cw_idx') > 0 AS merged;
SELECT count(*) < 80 AS merged_down FROM sift.col_segments('cw_idx');
SELECT sum(v), count(*) FROM cw;
SELECT sift.col_verify('cw_idx') IS NOT NULL AS verified;
DELETE FROM cw WHERE id % 3 = 0;
VACUUM cw;
SELECT sum(v), count(*) FROM cw;
SELECT sift.col_verify('cw_idx') IS NOT NULL AS verified;
SELECT sift._test_dir_check('cw_idx') > 0 AS chain_holds;

DROP PROCEDURE fill_wide(int), fill_cw(int);
DROP TABLE wide, cw;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
