-- a timestamp key of an index with 5,000 rows in its write buffer (written after the build, not promoted): the buffer is read in ranges of the key with the segment
-- label: `SELECT id, created_at ORDER BY created_at OFFSET rows / 10 LIMIT 10` with 5,000 rows in the write buffer
-- ordered: yes
-- vacuum: no
--@ setup
DROP TABLE IF EXISTS olw;
CREATE TABLE olw AS SELECT g::int AS id, timestamp '2024-01-01' + (hashint8extended(g, 7) & 35184372088831) * interval '1 microsecond' AS created_at FROM generate_series(1, {rows}) g ORDER BY md5((g * 31 + 7)::text);
CREATE INDEX olw_col ON olw USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE SCHEMA c_wbuf_layout_offset__whole;
CREATE TABLE c_wbuf_layout_offset__whole.olw AS TABLE olw;
CREATE INDEX olw_col ON c_wbuf_layout_offset__whole.olw USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'whole');
CREATE SCHEMA c_wbuf_layout_offset__compact;
CREATE TABLE c_wbuf_layout_offset__compact.olw AS TABLE olw;
CREATE INDEX olw_col ON c_wbuf_layout_offset__compact.olw USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at', order_layout = 'compact');
CREATE SCHEMA c_wbuf_layout_offset__btree;
CREATE TABLE c_wbuf_layout_offset__btree.olw AS TABLE olw;
CREATE INDEX olw_bt ON c_wbuf_layout_offset__btree.olw (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) olw;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_wbuf_layout_offset__whole.olw;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_wbuf_layout_offset__compact.olw;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_wbuf_layout_offset__btree.olw;
DO $$ DECLARE c float8 := (SELECT correlation FROM pg_stats WHERE schemaname = 'c_events' AND tablename = 'olw' AND attname = 'created_at'); BEGIN IF c IS NULL OR abs(c) >= 0.05 THEN RAISE EXCEPTION 'the key follows the heap: correlation %', c; END IF; END $$;
INSERT INTO olw SELECT g::int, timestamp '2024-01-01' + (hashint8extended(g, 7) & 35184372088831) * interval '1 microsecond' FROM generate_series({rows} + 1, {rows} + 5000) g;
INSERT INTO c_wbuf_layout_offset__whole.olw SELECT g::int, timestamp '2024-01-01' + (hashint8extended(g, 7) & 35184372088831) * interval '1 microsecond' FROM generate_series({rows} + 1, {rows} + 5000) g;
INSERT INTO c_wbuf_layout_offset__compact.olw SELECT g::int, timestamp '2024-01-01' + (hashint8extended(g, 7) & 35184372088831) * interval '1 microsecond' FROM generate_series({rows} + 1, {rows} + 5000) g;
INSERT INTO c_wbuf_layout_offset__btree.olw SELECT g::int, timestamp '2024-01-01' + (hashint8extended(g, 7) & 35184372088831) * interval '1 microsecond' FROM generate_series({rows} + 1, {rows} + 5000) g;
ANALYZE olw;
ANALYZE c_wbuf_layout_offset__whole.olw;
ANALYZE c_wbuf_layout_offset__compact.olw;
ANALYZE c_wbuf_layout_offset__btree.olw;
--@ pg
SELECT id, created_at FROM olw ORDER BY created_at OFFSET {rows} / 10 LIMIT 10
