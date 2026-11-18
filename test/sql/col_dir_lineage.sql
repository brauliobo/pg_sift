-- The partitions a forced merge of a key-cut index writes are partitions of the directory, each named by a lineage, and the steps that rewrite one (VACUUM after deletes) keep its lineage:
-- each partition records the bound it starts at (the first at the lowest key), and the rows of its segments lie between its bound and the next one;
-- no segment falls out of its partition into the entries of none, and a partition that lost all its rows is gone.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '60s';
CREATE TABLE t (id int, dim int, day date, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g;
VACUUM (FREEZE, ANALYZE) t;
SET sift.test_col_segment_kb = 64;
SET sift.col_auto_merge_partition_kb = 1536;
CREATE INDEX t_idx ON t USING sift_col (dim, day, v, w) WITH (key_cut = 'dim, day');
SELECT sift.col_merge('t_idx', true) > 0 AS merged;
CREATE TABLE before AS SELECT * FROM sift._test_dir_parts('t_idx');
SELECT count(*) AS partitions, count(*) FILTER (WHERE lo_empty) AS empty_bound, bool_and(rows_from_lo) AS rows_from_lo, bool_and(rows_below_next) AS rows_below_next FROM sift._test_col_part_bounds('t_idx');
SELECT (SELECT count(*) FROM sift.col_segments('t_idx') WHERE kind = 'immutable') = count(*) AS every_segment_in_a_partition, count(*) > 30 AS cut_in_partitions, count(DISTINCT lineage) = count(*) AS distinct_lineages,
	bool_and(lineage = first_gen) AS named_by_first_entry FROM before;
DELETE FROM t WHERE dim = 3;
VACUUM t;
SELECT (SELECT count(*) FROM sift.col_segments('t_idx') WHERE kind = 'immutable') = count(*) AS every_segment_in_a_partition, count(*) < (SELECT count(*) FROM before) AS emptied_partitions_gone,
	count(DISTINCT lineage) = count(*) AS distinct_lineages, bool_and(lineage IN (SELECT lineage FROM before)) AS lineages_kept FROM sift._test_dir_parts('t_idx');
SELECT count(*) AS partitions, count(*) FILTER (WHERE lo_empty) AS empty_bound, bool_and(rows_from_lo) AS rows_from_lo, bool_and(rows_below_next) AS rows_below_next FROM sift._test_col_part_bounds('t_idx');
SELECT sift.col_verify('t_idx', true);
DROP TABLE t, before;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
