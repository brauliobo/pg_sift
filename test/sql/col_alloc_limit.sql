-- The arrays a build allocates in proportion to the rows of a segment (the entries of a point directory, a sort copy of an order array) are bounded by the
-- memory budget of the build, not by the table: a writer counts the keys and addresses it keeps for them, with the arrays the end of the segment builds from them,
-- and cuts the segment at the budget. A key_order index that is not cut by a key (a group and a date) used to hold every row of the table in one segment, and
-- its order array asked for 24 bytes a row at the end of the build (an ERROR at 1 GB: 260 million rows after twelve minutes of scan). sift.test_col_segment_kb
-- lowers the budget (the index is built with key_cut = 'off': the build cuts a key_order index by its group, which has its own runs), so that a few rows reach it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;

CREATE TABLE al (id bigint PRIMARY KEY, tenant_id int, event_date date, updated_at timestamptz, deleted_at timestamptz, pad text) WITH (autovacuum_enabled = false);
INSERT INTO al SELECT g, (g * 7919 % 400)::int, date '2020-01-01' + (g % 2000), timestamptz '2020-01-01' + g * interval '7 seconds',
	CASE WHEN g % 50 = 0 THEN timestamptz '2021-01-01' END, md5(g::text) FROM generate_series(1, 200000) g;
VACUUM (FREEZE, ANALYZE) al;

-- the memory budget of a build: 2 MB; no segment holds the whole table (200,000 rows of 24 bytes is 4.8 MB)
SET sift.test_col_segment_kb = 2000;
CREATE INDEX al_col ON al USING sift_col (id, tenant_id, event_date, updated_at, deleted_at, pad) WITH (key_order = 'tenant_id, event_date', key_cut = 'off');
SELECT count(*) > 4 AS segments_by_budget, max(rows) * 24 < 4194304 AS arrays_under_limit FROM sift.col_segments('al_col') WHERE kind = 'immutable';
SELECT sift.col_verify('al_col', true);

-- the rows are the heap's
SET sift.enable_agg = on;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT count(*), sum(id), min(event_date) FROM al WHERE tenant_id = 17;
SELECT count(*) FILTER (WHERE deleted_at IS NOT NULL), count(*) FROM al;
SET sift.enable_agg = off;
SET enable_seqscan = on;
SELECT count(*), sum(id), min(event_date) FROM al WHERE tenant_id = 17;
SELECT count(*) FILTER (WHERE deleted_at IS NOT NULL), count(*) FROM al;

-- REINDEX CONCURRENTLY and a second build are bounded the same way
DROP INDEX al_col;
CREATE INDEX al_col ON al USING sift_col (id, tenant_id, event_date, updated_at, deleted_at, pad) WITH (key_order = 'tenant_id, event_date', key_cut = 'off');
REINDEX INDEX CONCURRENTLY al_col;
SELECT count(*) > 4 AS segments_by_budget, max(rows) * 24 < 4194304 AS arrays_under_limit FROM sift.col_segments('al_col') WHERE kind = 'immutable';
SELECT sift.col_verify('al_col', true);

DROP TABLE al;
DROP EXTENSION pg_sift;
