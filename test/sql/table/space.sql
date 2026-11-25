-- Space: one VACUUM of a sift_table gives the space back, not the second. The write buffers that a load fills are converted to column
-- segments, which VACUUM writes into the free blocks below the segments that end the file (the relation is locked exclusively for that,
-- when the lock is free: nothing reads or writes it, so what the VACUUM itself released is reusable at once) and then cuts the free end.
-- An emptied and refilled table stays the size it had, an update-heavy table returns to about its live size, and VACUUM FULL does not
-- grow it. vacuum_truncate = false keeps the file as it was.
CREATE EXTENSION pg_sift;
CREATE TABLE sp (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false);
-- a table nothing was written to has no metapage yet: no blocks, no sections
SELECT relation_blocks, high_water, free_blocks, reusable_blocks, free_extents FROM sift.table_space('sp');
SELECT count(*) AS sections FROM sift.col_sections('sp');
CREATE TEMP TABLE hw (round int, blocks bigint);
\set fill 'INSERT INTO sp SELECT g, g * 3, ''text '' || md5(g::text) FROM generate_series(1, 30000) g'
:fill;
INSERT INTO hw SELECT 0, relation_blocks FROM sift.table_space('sp');
-- the write buffers hold the load: the file is several times what its column segments need
VACUUM sp;
INSERT INTO hw SELECT 1, relation_blocks FROM sift.table_space('sp');
SELECT count(*) FILTER (WHERE kind = 'columns') > 0 AS promoted, count(*) FILTER (WHERE kind = 'sealed buffer') AS waiting FROM sift.table_segments('sp');
SELECT relation_blocks * 3 < (SELECT blocks FROM hw WHERE round = 0) AS compacted, relation_blocks = high_water AS at_mark,
	free_blocks * 20 < relation_blocks AS little_free FROM sift.table_space('sp');

-- updates rewrite rows into new places: VACUUM brings the file back near its live size
UPDATE sp SET v = v + 1 WHERE id % 2 = 0;
INSERT INTO hw SELECT 2, relation_blocks FROM sift.table_space('sp');
VACUUM sp;
SELECT relation_blocks * 2 < (SELECT blocks FROM hw WHERE round = 2) AS shrunk,
	relation_blocks <= 2 * (SELECT blocks FROM hw WHERE round = 1) AS near_live, free_blocks * 20 < relation_blocks AS little_free
	FROM sift.table_space('sp');
SELECT count(*), sum(v) FROM sp;

-- deletes
DELETE FROM sp WHERE id % 4 <> 1;
VACUUM sp;
SELECT relation_blocks < (SELECT blocks FROM hw WHERE round = 1) AS smaller, free_blocks * 20 < relation_blocks AS little_free FROM sift.table_space('sp');
SELECT count(*), sum(v) FROM sp;
DELETE FROM sp;
VACUUM sp;
SELECT relation_blocks * 4 < (SELECT blocks FROM hw WHERE round = 1) AS nearly_empty FROM sift.table_space('sp');
SELECT count(*) FROM sift.table_segments('sp') WHERE kind = 'columns';

-- refilled: the same size as the first time
:fill;
VACUUM sp;
SELECT relation_blocks <= 1.2 * (SELECT blocks FROM hw WHERE round = 1) AS reused FROM sift.table_space('sp');

VACUUM FULL sp;
SELECT relation_blocks * 3 < (SELECT blocks FROM hw WHERE round = 0) AS compact, (SELECT count(*) FROM sift.table_segments('sp') WHERE kind = 'columns') > 0 AS promoted FROM sift.table_space('sp');
VACUUM sp;
SELECT relation_blocks <= (SELECT blocks FROM hw WHERE round = 1) AS no_larger FROM sift.table_space('sp');
SELECT count(*) FROM sp;

-- vacuum_truncate off keeps the file; VACUUM (TRUNCATE) cuts it anyway
ALTER TABLE sp SET (vacuum_truncate = false);
:fill;
:fill;
VACUUM sp;
INSERT INTO hw SELECT 3, relation_blocks FROM sift.table_space('sp');
DELETE FROM sp;
VACUUM sp;
SELECT relation_blocks = (SELECT blocks FROM hw WHERE round = 3) AS kept FROM sift.table_space('sp');
VACUUM (TRUNCATE) sp;
SELECT relation_blocks < (SELECT blocks FROM hw WHERE round = 3) AS truncated FROM sift.table_space('sp');
DROP TABLE sp;

-- scattered deletes that leave every segment a third to a half dead: VACUUM rewrites them too, so the file follows the live rows
-- (it stays above them by the segments of the rewrite and the free end it keeps) and a refill takes the freed blocks
CREATE TABLE sc (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO sc SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series(1, 60000) g;
VACUUM sc;
CREATE TEMP TABLE hs AS SELECT relation_blocks AS fresh FROM sift.table_space('sc');
DELETE FROM sc WHERE id % 10 < 4;
VACUUM sc;
SELECT relation_blocks * 10 < fresh * 8 AS shrunk, relation_blocks * 10 > fresh * 5 AS holds_live, free_blocks * 5 < relation_blocks AS little_free
	FROM sift.table_space('sc'), hs;
INSERT INTO sc SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series(1, 60000) g WHERE g % 10 < 4;
VACUUM sc;
SELECT relation_blocks * 10 <= fresh * 12 AS refilled FROM sift.table_space('sc'), hs;
SELECT count(*), sum(v) FROM sc;
DROP TABLE sc;

-- a load in batches: each batch promotes its write buffers into a segment, the VACUUM merges them into one above the holes the buffers leave, in pieces that
-- skip past holes that this VACUUM released itself; the highest pieces go down into those holes and the free end is cut, as for an index
CREATE TABLE sb (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO sb SELECT g, g * 3, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, 150000) g;
INSERT INTO sb SELECT g, g * 3, md5(g::text) || md5((g * 7)::text) FROM generate_series(150001, 300000) g;
VACUUM sb;
SELECT free_blocks * 20 < relation_blocks AS little_free, relation_blocks = high_water AS at_mark FROM sift.table_space('sb');
SELECT count(*), sum(v) FROM sb;
DROP TABLE sb;
DROP EXTENSION pg_sift;
