-- Two-phase radix-partitioned parallel aggregation (src/agg/par/radix.c): a leader and 1, 2 and 4 workers aggregate the same rows
-- through their local tables, the shared partitions and the parallel merge; the groups that come back are compared field by field with a sorted reference.
-- Modes: 0 inline key, 1 wide key, 2 count(DISTINCT) per group, 3 ungrouped count(DISTINCT). A small local table forces many
-- flushes into the partitions; a budget of two chunks (or of work_mem at its minimum) sends partitions to the files of the SharedFileSet.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers = 8;

-- (workers, mode, local_cap): all launched, no wrong group, no spill
SELECT w AS workers, m AS mode, r.launched = w AS launched_all, r.bad AS wrong, r.spilled = 0 AS in_memory, r.out_groups
FROM (VALUES (1, 0, 4096), (2, 0, 128), (4, 0, 4096), (1, 1, 128), (2, 1, 4096), (4, 1, 128),
             (1, 2, 4096), (2, 2, 128), (4, 2, 4096), (1, 3, 4096), (2, 3, 128), (4, 3, 4096)) AS v(w, m, c),
     LATERAL sift._test_radix_par(7, w, 8, 3000, m, c, 256) AS r
ORDER BY w, m;

-- the chunks do not hold the partitions: two chunks, then the budget of work_mem and hash_mem_multiplier at their minimum
SELECT w AS workers, m AS mode, r.launched = w AS launched_all, r.bad AS wrong, r.spilled > 0 AS spilled
FROM (VALUES (1, 0), (2, 0), (4, 0), (2, 1), (4, 1), (2, 2), (4, 2), (2, 3), (4, 3)) AS v(w, m),
     LATERAL sift._test_radix_par(11, w, 12, 20000, m, 128, 2) AS r
ORDER BY w, m;

-- eight workers and the leader flush small local tables into the same partitions: the reservations of the chunks cross the end of a chunk
-- all the time and race for the next chunk of the pool, which runs out in the middle for the small pools
SELECT c AS chunks, m AS mode, r.launched = 8 AS launched_all, r.bad AS wrong, r.spilled > 0 AS spilled
FROM (VALUES (8, 0), (8, 1), (8, 2), (8, 3), (2000, 0), (2000, 2)) AS v(c, m),
     LATERAL sift._test_radix_par(17, 8, 24, 40000, m, 64, c) AS r
ORDER BY c, m;

SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT w AS workers, m AS mode, r.launched = w AS launched_all, r.bad AS wrong, r.spilled > 0 AS spilled, r.chunks
FROM (VALUES (2, 0), (4, 1), (4, 2), (2, 3)) AS v(w, m),
     LATERAL sift._test_radix_par(13, w, 12, 1000, m, 128, 0) AS r
ORDER BY w, m;

-- the groups of a partition are merged in memory, within that budget: more groups than it holds are an error with a message, not a wrong answer
\set VERBOSITY terse
SELECT * FROM sift._test_radix_par(13, 2, 12, 20000, 0, 128, 0);
SELECT * FROM sift._test_radix_par(13, 2, 12, 20000, 2, 128, 0);
\set VERBOSITY default

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
