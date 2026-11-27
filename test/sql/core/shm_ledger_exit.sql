-- The ledger of the shared memory pools (core/shm/budget.c) gets back, when a backend exits, the credit of what it released though it never drew a batch: bytes another
-- backend reserved and this one freed are not held by the pool for good. Needs dblink and pg_sift_test.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION IF NOT EXISTS dblink;
SELECT sift._test_shm_reserve(2, 1000) AS reserved;
CREATE TEMP TABLE base AS SELECT sift._test_shm_pool_used(2) AS used;

-- another backend frees the 1000 bytes and exits
SELECT dblink_connect('b', format('dbname=%s user=%s', current_database(), current_user));
CREATE TEMP TABLE b AS SELECT pid FROM dblink('b', 'SELECT pg_backend_pid()') AS t(pid int);
SELECT released FROM dblink('b', 'SELECT sift._test_shm_release(2, 1000)::text') AS t(released text);
SELECT (SELECT used FROM base) - sift._test_shm_pool_used(2) AS given_back_before_exit;
SELECT pg_terminate_backend(pid, 10000) AS exited FROM b;
SELECT dblink_disconnect('b');
SELECT (SELECT used FROM base) - sift._test_shm_pool_used(2) AS given_back;

DROP TABLE b, base;
DROP EXTENSION dblink;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
