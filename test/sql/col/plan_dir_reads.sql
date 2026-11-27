-- Planning a statement validates the statistics of each column index it considers against the directory of the index: the version of the directory page is read (one buffer
-- access for each index), and the metapage and the segments only when the version is not the one the statistics were made for. The statistics follow every change of the directory:
-- a write that is promoted, a REINDEX, an ALTER INDEX that changes the leaves the index keeps, a DROP INDEX. Counts of buffer accesses and of validations only, no timings.
CREATE EXTENSION pg_sift;
-- Format 1: the leaf statistics are those of a Format 1 index.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE pd (id int NOT NULL, a int NOT NULL, amount float8 NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO pd SELECT i, i % 7, i % 5 FROM generate_series(1, 6000) i;
CREATE INDEX pd_a ON pd USING sift_col (id, a, amount);
CREATE INDEX pd_b ON pd USING sift_col (id, a, amount);
CREATE INDEX pd_c ON pd USING sift_col (id, a, amount);
CREATE INDEX pd_d ON pd USING sift_col (id, a, amount);
VACUUM (FREEZE, ANALYZE) pd;

-- the validations of the statistics in the planning of q, the ones that read the directory in full, and the ones that folded the units again
CREATE FUNCTION plan_stats(q text, OUT checked bigint, OUT dir_read bigint, OUT built bigint) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM sift.stats_reset();
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP NULL; END LOOP;
	SELECT value INTO checked FROM sift.stats(local => true) WHERE name = 'leaf_stats_checked';
	SELECT value INTO dir_read FROM sift.stats(local => true) WHERE name = 'leaf_stats_dir_read';
	SELECT value INTO built FROM sift.stats(local => true) WHERE name = 'leaf_stats_built';
END $$;

\set q 'SELECT a, count(*), sum(amount) FROM pd WHERE id > 100 GROUP BY a'
-- a new backend: the VACUUM above validated the statistics in this one
\c -
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SELECT dir_read > 0 AS first_plan_of_the_backend_reads_directories FROM plan_stats(:'q');
SELECT checked = 4 AS indexes_validated, dir_read = 0 AS directories_not_read, built = 0 AS statistics_kept FROM plan_stats(:'q');

-- the statistics follow a write that the directory records, and a REINDEX
INSERT INTO pd SELECT i, i % 7, i % 5 FROM generate_series(6001, 9000) i;
SELECT sift.col_promote(i) >= 0 AS promoted FROM unnest(ARRAY['pd_a', 'pd_b', 'pd_c', 'pd_d']) i;
SELECT dir_read >= 4 AS directories_read_after_the_promotion, built >= 1 AS statistics_made_again FROM plan_stats(:'q');
SELECT dir_read = 0 AS then_not_read, built = 0 AS then_kept FROM plan_stats(:'q');
REINDEX INDEX pd_a;
SELECT dir_read >= 1 AS directory_read_after_reindex, built >= 1 AS statistics_made_again_after_reindex FROM plan_stats(:'q');
SELECT dir_read = 0 AS then_not_read_again, built = 0 AS then_kept_again FROM plan_stats(:'q');

-- an ALTER INDEX that changes the leaves of the index: its statistics are made again, once
ALTER INDEX pd_b SET (rollup_columns = 'off');
SELECT dir_read >= 1 AS directory_read_after_alter, built >= 1 AS statistics_made_again_after_alter FROM plan_stats(:'q');
SELECT dir_read = 0 AS then_not_read_after_alter, built = 0 AS then_kept_after_alter FROM plan_stats(:'q');

-- a DROP INDEX: the planning validates the indexes that are left, and reads nothing for them; an index made in its place has statistics of its own
DROP INDEX pd_d;
SELECT checked = 3 AS indexes_left_validated, dir_read = 0 AS directories_not_read, built = 0 AS statistics_kept FROM plan_stats(:'q');
CREATE INDEX pd_d ON pd USING sift_col (id, a, amount);
SELECT checked >= 4 AS indexes_validated, dir_read >= 1 AS new_directory_read, built >= 1 AS new_statistics_made FROM plan_stats(:'q');
SELECT dir_read = 0 AS then_not_read_new, built = 0 AS then_kept_new FROM plan_stats(:'q');

DROP FUNCTION plan_stats(text);
DROP TABLE pd;
DROP EXTENSION pg_sift;
