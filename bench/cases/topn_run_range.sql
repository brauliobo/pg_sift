-- the first rows after a point in time, ORDER BY a microsecond timestamp that arrives in no order LIMIT 10, no b-tree on it: the sorted run of a unit is searched for the first entry of the range, where the rows of the unit were all walked to it
-- label: `SELECT created_at WHERE created_at >= '2024-09-01' ORDER BY created_at LIMIT 10`, `order_columns = 'created_at'`, no b-tree; the variant `no_run` is the same index without the sorted runs
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_run_range_plain CASCADE;
CREATE SCHEMA topn_run_range_plain;
CREATE TABLE orders_rr AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_rr_col ON orders_rr USING sift_col (amount, created_at) WITH (order_columns = 'created_at');
CREATE TABLE topn_run_range_plain.orders_rr AS TABLE orders_rr;
CREATE INDEX orders_rr_col ON topn_run_range_plain.orders_rr USING sift_col (amount, created_at);
VACUUM (FREEZE, ANALYZE) orders_rr;
VACUUM (FREEZE, ANALYZE) topn_run_range_plain.orders_rr;
--@ pg
SELECT created_at FROM orders_rr WHERE created_at >= timestamp '2024-09-01' ORDER BY created_at LIMIT 10
