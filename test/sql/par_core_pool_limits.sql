-- The compute threads of a backend (core/par/pool.h) follow the settings that turn parallelism down: sift.parallel_scan = off, max_parallel_workers = 0 and a smaller
-- max_parallel_workers cap them, and a backend that has made its threads can still start a program (COPY ... PROGRAM).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 4;
SET max_parallel_workers = 8;

CREATE FUNCTION pool_run(w int) RETURNS TABLE (threads int, bad bigint) LANGUAGE sql AS
$$ SELECT r.threads, r.bad FROM sift._test_par_group(w, 200000, 3000, 2, 0) r $$;

SELECT * FROM pool_run(4) AS threads_of_default;

SET sift.parallel_scan = off;
SELECT * FROM pool_run(4) AS threads_without_parallel_scan;
RESET sift.parallel_scan;

SET max_parallel_workers = 0;
SELECT * FROM pool_run(4) AS threads_without_workers;

SET max_parallel_workers = 2;
SELECT * FROM pool_run(4) AS threads_with_two_workers;
RESET max_parallel_workers;

-- the threads are made now: a program the backend starts still runs, and the threads still work after it
SELECT * FROM pool_run(4) AS threads_before_program;
COPY (SELECT 1) TO PROGRAM 'cat >/dev/null';
SELECT * FROM pool_run(4) AS threads_after_program;

DROP FUNCTION pool_run(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
