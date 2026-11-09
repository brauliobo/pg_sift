-- sift.col_merge(index, true) merges to the minimum count of segments: the partitions the live bytes make of sift.col_auto_merge_partition_kb. An index
-- that is there (every segment a partition, none dirty, no more of them than the merge would write) is left as it is, so a second force does nothing: the
-- same segments, no WAL. A segment with a dead row, or a smaller one beside the partitions, is merged again.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge_partition_kb = 1024;

CREATE TABLE items (id int, kind text, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT g, 'cat_' || (g % 8), g % 997, md5(g::text) || md5(g::text || 'b') FROM generate_series(1, 60000) g;
SET sift.test_col_segment_kb = 512;
CREATE INDEX items_col ON items USING sift_col (id, kind, amount, pad) WITH (key_cut = 'off');
RESET sift.test_col_segment_kb;
VACUUM items;

CREATE FUNCTION im_segments() RETURNS bigint[] LANGUAGE sql AS
$$ SELECT array_agg(gen ORDER BY gen) FROM sift.col_segments('items_col') WHERE kind = 'immutable' $$;
CREATE FUNCTION im_lsn() RETURNS pg_lsn LANGUAGE sql AS $$ SELECT pg_current_wal_lsn() $$;

SELECT count(*) > 3 AS built_in_pieces FROM sift.col_segments('items_col') WHERE kind = 'immutable';

-- the first force merges the pieces into partitions
CREATE TEMP TABLE before AS SELECT im_segments() AS gens, im_lsn() AS lsn;
SELECT sift.col_merge('items_col', true) > 0 AS merged;
SELECT cardinality(im_segments()) < cardinality(gens) AS fewer, cardinality(im_segments()) > 1 AS partitions FROM before;
SELECT bool_and(bytes >= 1024 * 512) AS all_partitions FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT sift.col_verify('items_col', true);

-- the second finds the index at its minimum: nothing is written
DELETE FROM before;
INSERT INTO before SELECT im_segments(), im_lsn();
SELECT sift.col_merge('items_col', true) AS merges;
SELECT im_segments() = gens AS same_segments, pg_wal_lsn_diff(im_lsn(), lsn) < 65536 AS no_wal FROM before;
SELECT sift.col_merge('items_col', true) AS merges;
SELECT im_segments() = gens AS same_segments FROM before;

-- a dead row makes a partition worth writing again
DELETE FROM items WHERE id % 1000 = 0;
VACUUM items;
SELECT count(*) FILTER (WHERE dirty) > 0 AS dirty FROM sift.col_segments('items_col') WHERE kind = 'immutable';
SELECT sift.col_merge('items_col', true) > 0 AS merged;
SELECT im_segments() <> gens AS rewritten FROM before;
SELECT count(*) FILTER (WHERE dirty) AS dirty, sum(dead_rows) AS dead FROM sift.col_segments('items_col') WHERE kind = 'immutable';
DELETE FROM before;
INSERT INTO before SELECT im_segments(), im_lsn();
SELECT sift.col_merge('items_col', true) AS merges;
SELECT im_segments() = gens AS same_segments FROM before;

-- the answers are those of the table
SET sift.enable_agg = on;
SELECT kind, count(*), sum(amount) FROM items GROUP BY kind ORDER BY kind;
SET sift.enable_agg = off;
SELECT kind, count(*), sum(amount) FROM items GROUP BY kind ORDER BY kind;
SELECT sift.col_verify('items_col', true);

DROP TABLE items;
DROP FUNCTION im_segments();
DROP FUNCTION im_lsn();
DROP EXTENSION pg_sift;
