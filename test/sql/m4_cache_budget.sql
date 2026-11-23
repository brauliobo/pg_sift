-- What a backend keeps after its statements is bounded by sift.backend_cache_kb and by nothing a statement left behind: a part pin of a scan that failed is released, the state of an
-- index in the relcache and the snapshot of its manifest hold what they need and no page copies or arrays sized for the limits, and the caches that were outside the budget are in it.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE FUNCTION m4_named(n text) RETURNS bigint LANGUAGE sql AS
$$ SELECT count(*) FROM pg_backend_memory_contexts WHERE name = n $$;
SELECT current_setting('server_version_num')::int >= 180000 AS pg18 \gset
\if :pg18
CREATE FUNCTION m4_under_root(n text) RETURNS bigint LANGUAGE sql AS
$$ SELECT count(*) FROM pg_backend_memory_contexts c
   WHERE c.name = n AND c.path @> (SELECT path FROM pg_backend_memory_contexts WHERE name = 'SiftBackendCacheContext') $$;
\else
CREATE FUNCTION m4_under_root(n text) RETURNS bigint LANGUAGE sql AS
$$ WITH RECURSIVE t AS (SELECT name, parent FROM pg_backend_memory_contexts WHERE name = 'SiftBackendCacheContext'
                        UNION SELECT c.name, c.parent FROM pg_backend_memory_contexts c JOIN t ON c.parent = t.name)
   SELECT count(*) FROM t WHERE name = n $$;
\endif

-- a scan that fails holds no pin afterwards: with the cache emptied no part pin is left
CREATE TABLE m4_t (id int, v int) USING sift_table;
INSERT INTO m4_t SELECT i, i % 97 FROM generate_series(1, 20000) i;
VACUUM m4_t;
SELECT count(*), sum(v) FROM m4_t;
SELECT sum(1 / (v - 50)) FROM m4_t;
SET sift.backend_cache_kb = 0;
SELECT m4_named('sift cache f2 part pin') AS pins_left_after_the_error;
RESET sift.backend_cache_kb;

-- the pins are in the budget: under a small one they are given up when the statement ends, under the default they stay for the next
SET sift.backend_cache_kb = 32;
SELECT count(*), sum(v) FROM m4_t WHERE id < 5000;
SELECT m4_named('sift cache f2 part pin') AS pins_kept_under_a_small_budget;
RESET sift.backend_cache_kb;
SELECT count(*), sum(v) FROM m4_t WHERE id < 5000;
SELECT m4_under_root('sift cache f2 part pin') > 0 AS pins_kept_under_the_root_by_default;

-- the relcache state of an index with paths, and the snapshot of its manifest, are small
CREATE TABLE m4_d (id int PRIMARY KEY, g int, doc jsonb);
INSERT INTO m4_d SELECT i, i % 7, jsonb_build_object('a', i % 5, 'b', i % 3) FROM generate_series(1, 2000) i;
CREATE INDEX m4_d_col ON m4_d USING sift_col (id, g, doc) WITH (paths = 'doc.* int4');
SELECT count(*), sum(g) FROM m4_d WHERE (doc->>'a')::int < 3;
INSERT INTO m4_d VALUES (100000, 1, '{"a": 1}');
DELETE FROM m4_d WHERE id = 100000;
SELECT total_bytes <= 32 * 1024 AS index_state_is_small FROM pg_backend_memory_contexts WHERE name = 'index info' AND ident = 'm4_d_col';
SELECT m4_named('sift cache plan snapshot') > 0 AND (SELECT max(total_bytes) FROM pg_backend_memory_contexts WHERE name = 'sift cache plan snapshot') <= 16384 AS snapshot_is_small;

DROP TABLE m4_t, m4_d;
DROP FUNCTION m4_named(text), m4_under_root(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
