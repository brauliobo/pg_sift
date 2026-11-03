-- ORDER BY a key LIMIT 20 OFFSET a tenth of the rows over a table stored in the order of the key, with key_order: the variants are the same table with the default index and with a b-tree on the key
-- label: `SELECT id, ts ORDER BY id OFFSET <rows / 10> LIMIT 20` over a heap sorted by id, the index with `key_order = 'id'`; the variants `default` and `btree` are the same rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ko;
CREATE TABLE ko AS SELECT g::int AS id, (g % 997)::int AS a, (g % 101)::int AS b, (g * 7 % 1000)::int AS c, (g % 13)::int AS d, (g % 5)::int AS e, ((g % 1000) / 10.0)::numeric(8,2) AS amount, date '2024-01-01' + (g % 365) AS day, timestamp '2024-01-01' + g * interval '1 minute' AS ts, md5(g::text) AS tag, 'c' || (g % 50) AS cat FROM generate_series(1, {rows}) g ORDER BY g;
CREATE INDEX ko_col ON ko USING sift_col (id, a, b, c, d, e, amount, day, ts, tag, cat) WITH (key_order = 'id');
CREATE SCHEMA c_key_order_offset__default;
CREATE TABLE c_key_order_offset__default.ko AS TABLE ko;
CREATE INDEX ko_col ON c_key_order_offset__default.ko USING sift_col (id, a, b, c, d, e, amount, day, ts, tag, cat);
CREATE SCHEMA c_key_order_offset__btree;
CREATE TABLE c_key_order_offset__btree.ko AS TABLE ko;
CREATE UNIQUE INDEX ko_bt ON c_key_order_offset__btree.ko (id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ko;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_offset__default.ko;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_key_order_offset__btree.ko
--@ pg
SELECT id, ts FROM ko ORDER BY id OFFSET {rows} / 10 LIMIT 20
