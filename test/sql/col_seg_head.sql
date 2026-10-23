-- A segment never changes, so what opening it checks and parses from its header page, the schema and the directory of its units, is kept by the
-- backend for the next query (col/dictcache.c): the second scan of a segment reads neither, a rewritten segment has its own, a cache with no
-- room reads it every time, and the answers do not depend on any of it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE sh (id int, grp int, label text) WITH (autovacuum_enabled = off);
INSERT INTO sh SELECT g, g % 50, 'l' || g % 90 FROM generate_series(1, 30000) g;
CREATE INDEX sh_idx ON sh USING sift_col (id, grp, label) WITH (write_buffer_limit = 4096);
SELECT count(*) > 0 AS has_segment FROM sift.col_segments('sh_idx'::regclass) WHERE kind = 'immutable';

-- a scan finds the heads and the zone arrays of the segments the build and the last scan opened
SELECT rows FROM sift._test_col_counters('sh_idx', '1:eq:7');
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('sh_idx', '1:eq:7');
SELECT head_loads = 0 AS none_read, head_hits > 0 AS found, zone_loads = 0 AS no_zones_read, zone_hits > 0 AS zones_found FROM sift._test_col_dict_cache(true);

-- a scan that fails keeps nothing half made, and the next one finds the head
SET sift.enable_col_scan = on;
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS rows FROM sh WHERE grp = 7;
SELECT count(*) FROM sh WHERE grp = 7 AND 1 / (id - 7) > 0;
SELECT count(*) AS rows FROM sh WHERE grp = 7;
RESET enable_seqscan; RESET enable_bitmapscan;
RESET sift.enable_col_scan;
SELECT head_hits > 0 AS found FROM sift._test_col_dict_cache(true);

-- a segment written again (REINDEX) is another segment, found by the scans that follow, with the same answers
REINDEX INDEX sh_idx;
SELECT rows FROM sift._test_col_counters('sh_idx', '1:eq:7');
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('sh_idx', '1:eq:7');
SELECT head_loads = 0 AS none_read, head_hits > 0 AS found FROM sift._test_col_dict_cache(true);

-- a cache with no room for a head reads it every time, with the same answers
SET sift.test_limits = 'dict_cache_kb=1';
REINDEX INDEX sh_idx;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT rows FROM sift._test_col_counters('sh_idx', '1:eq:7');
SELECT rows FROM sift._test_col_counters('sh_idx', '1:eq:7');
SELECT head_loads >= 2 AS read_each_time, head_hits = 0 AS none_found, zone_loads >= 2 AS zones_read_each_time, zone_hits = 0 AS no_zones_found FROM sift._test_col_dict_cache(true);
RESET sift.test_limits;

SELECT sift.col_verify('sh_idx', true);
SELECT count(*) AS rows, sum(grp) AS total FROM sh WHERE grp BETWEEN 3 AND 9;

DROP TABLE sh;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
