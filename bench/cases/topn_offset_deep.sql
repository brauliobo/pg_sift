-- ORDER BY a timestamp that arrives in no order LIMIT 10 OFFSET 100000, past the 100,000 rows a top-N heap holds: the scan walks the sorted array of the key and holds the LIMIT only, where PostgreSQL sorts the table
-- label: `SELECT id, created_at ORDER BY created_at OFFSET 100000 LIMIT 10`, `order_array_columns = 'created_at'`; the variant `btree` is the same rows with a b-tree on the column
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_offset_deep_btree CASCADE;
CREATE SCHEMA topn_offset_deep_btree;
CREATE TABLE orders_deep AS SELECT id, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_deep_col ON orders_deep USING sift_col (id, created_at) WITH (order_columns = 'created_at', order_array_columns = 'created_at');
CREATE TABLE topn_offset_deep_btree.orders_deep AS TABLE orders_deep;
CREATE INDEX orders_deep_bt ON topn_offset_deep_btree.orders_deep (created_at);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) orders_deep;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) topn_offset_deep_btree.orders_deep;
--@ pg
SELECT id, created_at FROM orders_deep ORDER BY created_at OFFSET 100000 LIMIT 10
