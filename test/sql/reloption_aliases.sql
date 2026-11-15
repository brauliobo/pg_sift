-- rollup is rollup_leaves and rollup_dims is rollup_columns since 4.2; the older names stay accepted through 4.x. They are read as the new ones when the new one is empty,
-- the new one wins when both are set, CREATE INDEX and ALTER INDEX say which spelling to use, and the options of the index keep the name they were given.
-- (write_buffer_alias covers mutable_limit.)
CREATE EXTENSION pg_sift;
CREATE TABLE ra (id int, g int, h int, v int, items jsonb) WITH (autovacuum_enabled = off);
INSERT INTO ra SELECT i, i % 5, i % 7, i, jsonb_build_array(jsonb_build_object('kind', 'k' || i % 3)) FROM generate_series(1, 3000) i;

CREATE INDEX ra_old ON ra USING sift_col (id, g, h, v, items) WITH (paths = 'items[]{kind text}', rollup = 'items[].kind', rollup_dims = 'g, h: v');
CREATE INDEX ra_new ON ra USING sift_col (id, g, h, v, items) WITH (paths = 'items[]{kind text}', rollup_leaves = 'items[].kind', rollup_columns = 'g, h: v');
SELECT relname, reloptions FROM pg_class WHERE relname IN ('ra_old', 'ra_new') ORDER BY relname;
-- the same options: the same rollups (the unit records of the build) and the same verdict
SELECT (SELECT string_agg(format('%s/%s', segments, rows), ',') FROM sift.col_index_info('ra_old')) = (SELECT string_agg(format('%s/%s', segments, rows), ',') FROM sift.col_index_info('ra_new')) AS same_shape;
SELECT sift.col_verify('ra_old', true), sift.col_verify('ra_new', true);

-- both given: the new name decides (the old one names a column that is not there, and would fail alone)
CREATE INDEX ra_both ON ra USING sift_col (id, g, h, v) WITH (rollup_columns = 'off', rollup_dims = 'nope, g');
CREATE INDEX ra_alone ON ra USING sift_col (id, g, h, v) WITH (rollup_dims = 'nope, g');
CREATE INDEX ra_leaf_alone ON ra USING sift_col (id, items) WITH (paths = 'items[]{kind text}', rollup = 'items[].nope');
-- both words of auto
CREATE INDEX ra_auto ON ra USING sift_col (id, g, h, v) WITH (rollup_columns = 'auto');
ALTER INDEX ra_auto SET (rollup_leaves = 'default');

-- ALTER INDEX SET and RESET of the older names
ALTER INDEX ra_old SET (rollup = 'off');
ALTER INDEX ra_old SET (rollup_dims = 'g, h');
SELECT reloptions FROM pg_class WHERE relname = 'ra_old';
ALTER INDEX ra_old SET (rollup_leaves = 'items[].kind');
ALTER INDEX ra_old RESET (rollup, rollup_dims);
SELECT reloptions FROM pg_class WHERE relname = 'ra_old';
REINDEX INDEX ra_old;
SELECT sift.col_verify('ra_old', true);

DROP TABLE ra;
DROP EXTENSION pg_sift;
