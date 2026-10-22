-- The strings of one leaf in one column unit have a byte budget (1 GB). Past it the values are stored as overflow (NULL and flagged, read from
-- the heap like a value the leaf cannot store) and a NOTICE names the leaf: CREATE INDEX, a merge and a promotion no longer raise. sift.test_limits
-- lowers the budget so that a few thousand rows reach it. Every query runs as the normal plan and as the scan; the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

\ir test/agg_helpers.sql

\ir test/agg_same_helpers.sql

CREATE TABLE wv (id int, grp int, payload text, note text, bin bytea) WITH (autovacuum_enabled = false);
INSERT INTO wv SELECT i, i % 7, repeat(md5(i::text), 6), repeat(md5((i % 50)::text), 10), decode(repeat(md5(i::text), 8), 'hex')
FROM generate_series(1, 3000) i;

-- the build: three string leaves pass 64 kB in the one unit of 3000 rows (192, 320 and 128 bytes a row)
SET sift.test_limits = 'unit_bytes=65536';
CREATE INDEX wv_col ON wv USING sift_col (id, grp, payload, note, bin);
RESET sift.test_limits;
VACUUM (FREEZE, ANALYZE) wv;
SELECT sift.col_verify('wv_col', true);

-- the leaves without overflow are scanned; the others keep the normal plan, with the same answers
CREATE TEMP TABLE queries (q text);
INSERT INTO queries VALUES
	('SELECT grp, count(*), sum(id) FROM wv GROUP BY grp'),
	('SELECT grp, count(*) FROM wv WHERE id > 1000 GROUP BY grp'),
	('SELECT count(*), count(DISTINCT payload) FROM wv'),
	('SELECT grp, count(*), min(length(note)) FROM wv GROUP BY grp'),
	('SELECT note, count(*) FROM wv GROUP BY note'),
	('SELECT grp, count(*) FROM wv WHERE payload > repeat(''7'', 10) GROUP BY grp'),
	('SELECT grp, sum(length(bin)) FROM wv GROUP BY grp');
SELECT q, pushed(q) AS scanned, same(q) FROM queries ORDER BY q;

-- a merge decodes the units of the segments into one batch: past the budget it overflows the values too, and the promotion of rows written
-- after the build does the same for its own unit
CREATE TABLE wm (id int, grp int, payload text, bin bytea) WITH (autovacuum_enabled = false);
INSERT INTO wm SELECT i, i % 7, repeat(md5(i::text), 6), decode(repeat(md5(i::text), 8), 'hex') FROM generate_series(1, 1500) i;
CREATE INDEX wm_col ON wm USING sift_col (id, grp, payload, bin) WITH (write_buffer_limit = 4096);
INSERT INTO wm SELECT i, i % 7, repeat(md5(i::text), 6), decode(repeat(md5(i::text), 8), 'hex') FROM generate_series(1501, 3000) i;
SELECT pushed('SELECT grp, count(*), count(DISTINCT payload) FROM wm GROUP BY grp') AS scanned_before;
SET sift.test_limits = 'unit_bytes=65536';
SELECT sift.col_merge('wm_col', true) AS merges;
RESET sift.test_limits;
SELECT sift.col_verify('wm_col', true);
SELECT pushed('SELECT grp, count(*), count(DISTINCT payload) FROM wm GROUP BY grp') AS scanned_after,
	pushed('SELECT grp, count(*), sum(id) FROM wm GROUP BY grp') AS scanned_ids;
SELECT same('SELECT grp, count(*), count(DISTINCT payload) FROM wm GROUP BY grp'), same('SELECT grp, count(*), sum(id) FROM wm GROUP BY grp'),
	same('SELECT grp, sum(length(bin)) FROM wm GROUP BY grp');

-- a unit under the budget is untouched: no NOTICE, no overflow
CREATE TABLE ws (id int, payload text) WITH (autovacuum_enabled = false);
INSERT INTO ws SELECT i, repeat(md5(i::text), 6) FROM generate_series(1, 200) i;
SET sift.test_limits = 'unit_bytes=65536';
CREATE INDEX ws_col ON ws USING sift_col (id, payload);
RESET sift.test_limits;
SELECT pushed('SELECT count(DISTINCT payload) FROM ws') AS scanned, same('SELECT count(DISTINCT payload) FROM ws');

DROP TABLE wv, wm, ws;
DROP FUNCTION runq(text, bool), same(text), pushed(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
DROP FUNCTION outcome(text,boolean), runs_in_scan(text);
