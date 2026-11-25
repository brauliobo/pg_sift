-- The partitions a forced merge of a key_cut sift_col index writes follow sift.test_col_partition_kb: as many as the live bytes make of three quarters of the
-- setting (a partition is cut at a unit, so a setting under the bytes of a unit is the unit), whatever their number, and the directory of those partitions is a root and
-- one leaf, not a page for each partition.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE kn (id int, cust int, amount int) WITH (autovacuum_enabled = false);
CREATE INDEX kn_idx ON kn USING sift_col (id, cust, amount) WITH (key_cut = 'cust');
INSERT INTO kn SELECT i, ((i::bigint * 7919) % 2000)::int, i % 100 FROM generate_series(1, 100000) i;
SELECT sift.col_promote('kn_idx');
INSERT INTO kn SELECT i, ((i::bigint * 7919) % 2000)::int, i % 100 FROM generate_series(100001, 200000) i;
SELECT sift.col_promote('kn_idx');
INSERT INTO kn SELECT i, ((i::bigint * 7919) % 2000)::int, i % 100 FROM generate_series(200001, 300000) i;
SELECT sift.col_promote('kn_idx');
INSERT INTO kn SELECT i, ((i::bigint * 7919) % 2000)::int, i % 100 FROM generate_series(300001, 400000) i;
SELECT sift.col_promote('kn_idx');
SELECT count(*) AS runs, sum(rows) AS rows FROM sift.col_segments('kn_idx') WHERE kind = 'immutable';

CREATE FUNCTION merged_at(kb int) RETURNS TABLE (partitions_follow_the_setting bool, partitions int, directory_is_root_and_one_leaf bool, all_rows bool) LANGUAGE plpgsql AS $$
DECLARE live bigint; made int;
BEGIN
	SELECT sum(bytes) INTO live FROM sift.col_segments('kn_idx') WHERE kind = 'immutable';
	PERFORM set_config('sift.test_col_partition_kb', kb::text, false);
	PERFORM sift.col_merge('kn_idx', true);
	SELECT count(*) INTO made FROM sift.col_segments('kn_idx') WHERE kind = 'immutable';
	RETURN QUERY SELECT made = ceil(live / (kb * 768.0))::int, made, (SELECT chain_blocks = 2 FROM sift._test_dir_info('kn_idx')),
		(SELECT sum(rows) FROM sift.col_segments('kn_idx') WHERE kind = 'immutable') = (SELECT count(*) FROM kn);
END $$;

-- the first merge cuts four runs of overlapping keys by their units; the merges after it are of the partitions the bytes make
SELECT partitions > 1 AS runs_cut_into_partitions, all_rows FROM merged_at(256);
SELECT * FROM merged_at(512);
SELECT * FROM merged_at(1024);
SELECT * FROM merged_at(256);
SELECT sift.col_verify('kn_idx', true);

DROP FUNCTION merged_at(int);
DROP TABLE kn;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
