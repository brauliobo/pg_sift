-- The lineage of a partition across the steps that rewrite it: a rewrite keeps the lineage of each partition it replaces, a split keeps it for the part that holds the tail of the range
-- (the last) and names the others by the generation of their first entry, a merge keeps the lowest. The entries of no partition stay in front of the partitions, one lineage names one partition.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_dir_leaf_max = 3;
SET sift.test_dir_root_max = 8;
CREATE TABLE dm (v int);
CREATE INDEX dm_idx ON dm USING sift_store_test (v);
CREATE TABLE lineages (step text, first int, lineage int8);
CREATE FUNCTION note(step text) RETURNS void LANGUAGE sql AS $$ INSERT INTO lineages SELECT step, first, lineage FROM sift._test_dir_parts('dm_idx') $$;

-- 2 entries of no partition, then 4 that become partitions: each is named by the generation of its first entry
SELECT sift._test_dir_edit('dm_idx', 'ins', 0, 6) > 0 AS inserted;
SELECT sift._test_dir_edit('dm_idx', 'part', 2, 4) IS NOT NULL AS partitioned;
SELECT first, n, lineage = first_gen AS named_by_first_entry FROM sift._test_dir_parts('dm_idx') ORDER BY first;
SELECT note('made');

-- a rewrite of all four keeps each lineage
SELECT sift._test_dir_edit('dm_idx', 'part', 2, 4) IS NOT NULL AS rewritten;
SELECT note('rewritten');
SELECT count(*) AS partitions, bool_and(a.lineage = b.lineage) AS keep_their_lineage FROM lineages a JOIN lineages b USING (first) WHERE a.step = 'made' AND b.step = 'rewritten';

-- a split of the partition at 3 in 3: the last keeps the lineage, the others are named by their first entry
SELECT sift._test_dir_edit('dm_idx', 'split', 3, 3) IS NOT NULL AS split;
SELECT note('split');
SELECT p.first, p.n, p.lineage = (SELECT lineage FROM lineages WHERE step = 'rewritten' AND first = 3) AS keeps_the_old_lineage, p.lineage = p.first_gen AS named_by_first_entry
FROM sift._test_dir_parts('dm_idx') p WHERE p.first BETWEEN 3 AND 5 ORDER BY p.first;

-- a merge of the 3 partitions from 2: the lowest lineage survives
SELECT sift._test_dir_edit('dm_idx', 'merge', 2, 3) IS NOT NULL AS merged;
SELECT note('merged');
SELECT (SELECT lineage FROM lineages WHERE step = 'merged' AND first = 2) = (SELECT min(lineage) FROM lineages WHERE step = 'split' AND first BETWEEN 2 AND 4) AS lowest_survives;

-- one lineage names one partition, the entries of no partition are in front of them, and the blocks of the chain are sound
SELECT count(*) = count(DISTINCT lineage) AS distinct_lineages, min(first) AS first_partition, sum(n) AS entries_in_partitions FROM sift._test_dir_parts('dm_idx');
SELECT nsegs, chained FROM sift._test_dir_info('dm_idx');
SELECT sift._test_dir_check('dm_idx') > 0 AS chain_sound;
-- the lower bound of a partition, from a key bound of two leaves (integer, least, greatest, NULL), is what the chain holds when read back; partitions made with no bound hold none;
-- a bound of the first leaf alone reads as the lowest key of the second
CREATE TABLE dn (v int);
CREATE INDEX dn_idx ON dn USING sift_store_test (v);
SELECT sift._test_dir_edit('dn_idx', 'ins', 0, 12) > 0 AS inserted;
SELECT sift._test_dir_edit('dn_idx', 'part', 2, 10) IS NOT NULL AS partitioned_unbounded;
SELECT count(*) AS partitions, count(*) FILTER (WHERE lo_round_trips) AS round_trip FROM sift._test_dir_part_bounds('dn_idx');
SELECT sift._test_dir_edit('dn_idx', 'bound', 2, 10) IS NOT NULL AS partitioned_bounded;
SELECT count(*) AS partitions, count(*) FILTER (WHERE lo_round_trips) AS round_trip, count(*) FILTER (WHERE short_bound_lowest) AS short_reads_lowest FROM sift._test_dir_part_bounds('dn_idx');
SELECT sift._test_dir_check('dn_idx') > 0 AS chain_sound;
DROP TABLE dn;
DROP TABLE dm, lineages;
DROP FUNCTION note(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
