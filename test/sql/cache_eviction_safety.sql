-- The caches of a backend (core/cachebudget.h) give room to each other: a cache that builds an entry asks for room, and the entry it asks for must not be the one given up. Under a budget too
-- small for what a statement builds, the statistics of an index and the facts of a planning come out as they do under the default one; a unit buffer that cannot be made leaves nothing behind
-- for the next segment; and the table of cache kinds has room for every cache of the tree and more.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

SELECT sift._test_unit_room_failure() AS next_segment_buffer_is_whole;

-- one row to a heap page (a value of 7000 bytes that cannot be compressed or moved out of line), 256 pages to a unit: 6 units for each segment
CREATE TABLE es (id int, kind int, val bigint, pad bytea) WITH (autovacuum_enabled = off);
ALTER TABLE es ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO es SELECT g, g % 6, g * 7, decode(repeat(md5(g::text), 437), 'hex') FROM generate_series(1, 1536) g;
CREATE INDEX es_idx ON es USING sift_col (id, kind, val) WITH (write_buffer_limit = 4096, superunit = 0);
VACUUM (freeze, disable_page_skipping) es;

-- the same answers under the default budget, under a small one, and with every ask for room dropping everything nobody pins (sift._test_cache_stress): the entry a cache is building is not dropped
SELECT units, dim_units FROM sift._test_col_pre_stats('es_idx') WHERE leaf = 1;
SELECT kind, count(*), sum(val) FROM es GROUP BY kind ORDER BY kind;
\connect
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.backend_cache_kb = 64;
SELECT units, dim_units FROM sift._test_col_pre_stats('es_idx') WHERE leaf = 1;
SELECT same FROM sift._test_leaf_fold('es_idx');
SELECT kind, count(*), sum(val) FROM es GROUP BY kind ORDER BY kind;
RESET sift.backend_cache_kb;
\connect
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SELECT sift._test_cache_stress(true) AS stress;
SELECT units, dim_units FROM sift._test_col_pre_stats('es_idx') WHERE leaf = 1;
SELECT same FROM sift._test_leaf_fold('es_idx');
SELECT kind, count(*), sum(val) FROM es GROUP BY kind ORDER BY kind;
WITH a AS (SELECT kind, count(*) AS n FROM es GROUP BY kind), b AS (SELECT kind, sum(val) AS s FROM es GROUP BY kind) SELECT a.kind, a.n, b.s FROM a JOIN b USING (kind) ORDER BY a.kind;
SELECT kind, count(*), sum(val) FROM es WHERE id < 800 GROUP BY kind ORDER BY kind;
SELECT id, val FROM es ORDER BY val DESC, id LIMIT 3;
SELECT id, kind FROM es ORDER BY kind DESC, id DESC LIMIT 3;
-- rows in the write buffer of the index: found by a lookup of their key
INSERT INTO es SELECT g, g % 6, g * 7, decode(repeat(md5(g::text), 437), 'hex') FROM generate_series(1537, 1600) g;
SELECT count(*), sum(val) FROM es WHERE id IN (1540, 1550, 1599, 7);
SELECT kind, count(*) FROM es WHERE id > 1536 GROUP BY kind ORDER BY kind;
SELECT sift._test_cache_stress(false) AS stress;

-- the table of kinds: room for 30 more caches than the tree has. The registry is the backend's, so the test ends its session
SELECT sift._test_cache_kinds_max() >= 32 AS room_for_every_cache;
SELECT sift._test_cache_register_dummies(30) AS kinds_registered;
\connect

DROP TABLE es;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
