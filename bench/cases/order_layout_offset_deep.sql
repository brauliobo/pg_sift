-- ORDER BY a timestamp that arrives in no order with the layout the index chooses (order_layout auto): the variants are the same index with the whole order, with the compact order and a b-tree on the column
-- label: `SELECT id, created_at ORDER BY created_at OFFSET <OFFSET rows * 9 / 10 (900,000 at 1,000,000 rows)> LIMIT 10`, the index with `order_layout = auto`; the variants `whole`, `compact` and `btree` are the same rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS olx;
CREATE TABLE olx AS SELECT g::int AS id, timestamp '2024-01-01' + (hashint8extended(g, 7) & 35184372088831) * interval '1 microsecond' AS created_at FROM generate_series(1, {rows}) g ORDER BY md5((g * 31 + 7)::text);
CREATE INDEX olx_col ON olx USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE SCHEMA c_order_layout_offset_deep__whole;
CREATE TABLE c_order_layout_offset_deep__whole.olx AS TABLE olx;
CREATE INDEX olx_col ON c_order_layout_offset_deep__whole.olx USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'whole');
CREATE SCHEMA c_order_layout_offset_deep__compact;
CREATE TABLE c_order_layout_offset_deep__compact.olx AS TABLE olx;
CREATE INDEX olx_col ON c_order_layout_offset_deep__compact.olx USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact');
CREATE SCHEMA c_order_layout_offset_deep__btree;
CREATE TABLE c_order_layout_offset_deep__btree.olx AS TABLE olx;
CREATE INDEX olx_bt ON c_order_layout_offset_deep__btree.olx (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) olx;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_offset_deep__whole.olx;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_offset_deep__compact.olx;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_order_layout_offset_deep__btree.olx;
DO $$ DECLARE c float8 := (SELECT correlation FROM pg_stats WHERE schemaname = 'c_events' AND tablename = 'olx' AND attname = 'created_at'); BEGIN IF c IS NULL OR abs(c) >= 0.05 THEN RAISE EXCEPTION 'the key follows the heap: correlation %', c; END IF; END $$;
--@ pg
SELECT id, created_at FROM olx ORDER BY created_at OFFSET {rows} * 9 / 10 LIMIT 10
