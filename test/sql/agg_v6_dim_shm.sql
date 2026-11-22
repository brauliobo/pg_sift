-- Shared dimension table (src/agg/par/dim_shm.c): the leader and 0, 1, 2 and 4 workers build one immutable hash of the keys of a
-- dimension in a shared segment (count, prefix sum and fill, each by every participant), then probe it, and a worker writes its
-- answers to a file of the SharedFileSet of the context. Every answer, every probe once, and every key of the table are compared
-- with a sorted reference of the same rows. A key in several rows (a domain smaller than the rows) has all its rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers = 8;
SET max_parallel_workers_per_gather = 8;

-- (workers, rows, domain): unique keys (domain >> rows), some duplicates, many duplicates
SELECT w AS workers, n AS rows, d AS domain, r.launched = w AS launched_all, r.fits, r.answered = 20000 AS all_probes, r.wrong
FROM (VALUES (0, 50000, 1000000), (1, 50000, 1000000), (2, 50000, 1000000), (4, 50000, 1000000),
             (0, 30000, 20000), (2, 30000, 20000), (4, 30000, 20000),
             (1, 20000, 200), (4, 20000, 200), (4, 1, 10), (2, 17, 5)) AS v(w, n, d),
     LATERAL sift._test_dim_shm(7, w, n, d, 20000) AS r
ORDER BY w, n, d;

-- a dimension of many blocks of buckets and granules of rows
SET work_mem = '64MB';
SELECT w AS workers, r.launched = w AS launched_all, r.buckets, r.answered = 5000 AS all_probes, r.wrong
FROM (VALUES (1), (4)) AS v(w), LATERAL sift._test_dim_shm(11, w, 300000, 1000000000, 5000) AS r
ORDER BY w;

-- a table over the share of work_mem a shared dimension may take is not built
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT r.fits, r.launched, r.answered, r.wrong FROM sift._test_dim_shm(7, 2, 5000, 100000, 1000) AS r;
RESET work_mem;
RESET hash_mem_multiplier;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
