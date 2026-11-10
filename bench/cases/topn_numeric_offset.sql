-- ORDER BY a numeric(12, 2) column LIMIT 20 OFFSET 10000, the key scattered over the heap: the sorted array of the numeric key (its number scaled by 100) throws the entries before the OFFSET away, where the sorted runs read every row of the units
-- label: `SELECT id, amount ORDER BY amount, id LIMIT 20 OFFSET 10000` over a `numeric(12, 2)` column, `order_array_columns = 'amount'`; the variant `btree` is the same rows with a b-tree on (amount, id)
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_numeric_btree CASCADE;
CREATE SCHEMA topn_numeric_btree;
CREATE TABLE orders_num AS SELECT id, ((hashint8extended(id, 11) & 16777215) / 100.0)::numeric(12, 2) AS amount FROM orders;
CREATE INDEX orders_num_col ON orders_num USING sift_col (id, amount) WITH (order_array_columns = 'amount');
CREATE TABLE topn_numeric_btree.orders_num AS TABLE orders_num;
CREATE INDEX orders_num_bt ON topn_numeric_btree.orders_num (amount, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) orders_num;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) topn_numeric_btree.orders_num;
--@ pg
SELECT id, amount FROM orders_num ORDER BY amount, id LIMIT 20 OFFSET 10000
