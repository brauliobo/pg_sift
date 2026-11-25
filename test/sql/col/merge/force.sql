-- sift.col_merge(index, true) seals the open write buffer first, so one call is col_promote + col_merge: the rows written since the last
-- promotion are in the one segment it makes, not left as in-memory units next to it. The plain col_merge does not seal.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE items (id int, kind text, amount numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT g, 'cat_' || (g % 4), g % 997 FROM generate_series(1, 20000) g;
CREATE INDEX items_col ON items USING sift_col (id, kind, amount) WITH (write_buffer_limit = 4096);
VACUUM items;

-- rows written after the build sit in the open write buffer, below the limit that would seal it
INSERT INTO items SELECT g, 'cat_' || (g % 4), g % 997 FROM generate_series(20001, 20500) g;
SELECT kind, rows FROM sift.col_segments('items_col') ORDER BY kind, rows;

-- the plain merge asks the policy only: nothing to merge, and the buffer stays open
SELECT sift.col_merge('items_col') AS merges;
SELECT kind, rows FROM sift.col_segments('items_col') ORDER BY kind, rows;

-- the forced merge: one segment of every row, the fresh open buffer empty
SELECT sift.col_merge('items_col', true) AS merges;
SELECT kind, rows FROM sift.col_segments('items_col') ORDER BY kind, rows;
SELECT buffers, rows, units FROM sift.col_index_info('items_col');

-- the answers are those of the table
SET sift.enable_agg = on;
SELECT kind, count(*), sum(amount) FROM items GROUP BY kind ORDER BY kind;
SET sift.enable_agg = off;
SELECT kind, count(*), sum(amount) FROM items GROUP BY kind ORDER BY kind;
SELECT sift.col_verify('items_col', true);

-- an empty buffer is not sealed again: a second forced merge finds one segment and merges nothing
SELECT sift.col_merge('items_col', true) AS merges;
SELECT kind, rows FROM sift.col_segments('items_col') ORDER BY kind, rows;

DROP TABLE items;
DROP EXTENSION pg_sift;
