-- The space step of VACUUM moves the highest runs of a segment larger than sift.vacuum_rewrite_limit down into the free blocks below it, a round at a time,
-- and cuts the file: a merge writes its one segment above the blocks of the segments it replaced, and the file stays twice the live bytes until then.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE sh (id int, k int, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sh SELECT i, i % 500, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 60000) i;
CREATE INDEX sh_idx ON sh USING sift_col (id, k, amount, pad);
INSERT INTO sh SELECT i, i % 500, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(60001, 120000) i;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) sh;
SELECT sift.col_promote('sh_idx') >= 0 AS promoted;
SELECT sift.col_merge('sh_idx', true) > 0 AS merged;
CREATE TEMP TABLE want AS SELECT count(*) AS n, sum(amount) AS s, sum(length(pad)) AS l, count(DISTINCT k) AS d FROM sh;

CREATE FUNCTION sh_state() RETURNS TABLE (segments int, file_x100 int, answers bool) LANGUAGE sql AS $$
	SELECT (SELECT count(*)::int FROM sift.col_segments('sh_idx') WHERE kind = 'immutable'),
	       (SELECT (100 * relation_blocks * 8192 / (SELECT sum(bytes) FROM sift.col_sections('sh_idx') WHERE section <> 'free'))::int FROM sift.col_index_info('sh_idx')),
	       (SELECT (n, s, l, d) = (SELECT count(*), sum(amount), sum(length(pad)), count(DISTINCT k) FROM sh) FROM want) $$;

-- one segment, the file about twice its bytes
SELECT segments, file_x100 > 170 AS file_twice_the_segment, answers FROM sh_state();

-- a VACUUM with a one-megabyte allowance: the segment is a dozen times larger, it cannot move whole, it shifts
SET sift.vacuum_rewrite_limit = 1;
VACUUM sh;
SELECT segments, file_x100 < 115 AS file_near_the_segment, answers FROM sh_state();
SELECT sift.col_verify('sh_idx', true);

-- the shifted segment reads, updates and vacuums like any other
UPDATE sh SET amount = amount + 1 WHERE id % 7 = 0;
TRUNCATE want;
INSERT INTO want SELECT count(*), sum(amount), sum(length(pad)), count(DISTINCT k) FROM sh;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) sh;
SELECT answers FROM sh_state();
SELECT sift.col_verify('sh_idx', true);

DROP TABLE sh;
DROP FUNCTION sh_state();
DROP EXTENSION pg_sift;
