-- The ledger of the shared memory pools (core/shm/budget.c): five pools that share sift.shared_cache_kb, counted with atomics. A request under 64 kB is taken from a credit
-- the backend draws a batch at a time, so it touches no shared counter; a larger one, and the credit itself, is held by the pool up to its share of the setting and by the
-- pools together up to the setting.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.shared_cache_kb = 4096;
CREATE TEMP TABLE base AS SELECT sift._test_shm_pool_used(2) AS dimmap, sift._test_shm_pool_used(3) AS dict, sift._test_shm_pool_used(4) AS leaf, sift._test_shm_total() AS total;
CREATE FUNCTION held(pool int) RETURNS int8 LANGUAGE sql AS
  $$ SELECT sift._test_shm_pool_used(pool) - (SELECT CASE pool WHEN 2 THEN dimmap WHEN 3 THEN dict ELSE leaf END FROM base) $$;

-- a small request draws a batch of 64 kB, the next ones are served from it
SELECT sift._test_shm_reserve(4, 4096) AS reserved, held(4) AS batch;
SELECT sift._test_shm_reserve(4, 4096) AS reserved, held(4) AS same_batch;
SELECT sift._test_shm_release(4, 4096), sift._test_shm_release(4, 4096), held(4) AS batch_kept;

-- a request over what the credit holds draws another batch, and the credit above two batches goes back
SELECT sift._test_shm_reserve(4, 60000) AS reserved, held(4) AS one_batch;
SELECT sift._test_shm_reserve(4, 60000) AS reserved, held(4) AS two_batches;
SELECT sift._test_shm_release(4, 60000), sift._test_shm_release(4, 60000), held(4) AS one_batch_back;

-- a request of a batch or more is the pool's own, up to its share of the setting (a tenth for the leaf statistics)
SELECT sift._test_shm_reserve(4, 200000) AS reserved, held(4) AS with_request;
SELECT sift._test_shm_reserve(4, 200000) AS reserved_past_share, held(4) AS refused;
SELECT sift._test_shm_release(4, 200000), held(4) AS released;

-- the pools together may not pass the setting, whatever the share of one
SELECT sift._test_shm_reserve(3, 1000000) AS reserved, sift._test_shm_total() - (SELECT total FROM base) AS total;
SELECT set_config('sift.shared_cache_kb', (sift._test_shm_total() / 1024 + 64)::text, false) IS NOT NULL AS limit_just_over_what_is_held;
SELECT sift._test_shm_reserve(2, 100000) AS reserved_past_total, held(2) AS refused;
SELECT sift._test_shm_release(3, 1000000), held(3) AS released;
SELECT sift._test_shm_reserve(2, 100000) AS reserved_within_total, sift._test_shm_release(2, 100000), held(2) AS released;

-- an unknown pool
SELECT sift._test_shm_reserve(5, 1);

DROP FUNCTION held(int);
DROP TABLE base;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
