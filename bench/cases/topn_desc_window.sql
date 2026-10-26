-- the newest rows before a point in time, ORDER BY a timestamp that grows with the table DESC LIMIT 10, no b-tree on it: the superblocks of a window of the walk are read from its best end, where a window holding the rows of the page read every unit of it
-- label: `SELECT id, created_at WHERE created_at <= '2024-09-01' ORDER BY created_at DESC LIMIT 10`, `order_columns = 'created_at'`, no b-tree
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS orders_dw;
CREATE TABLE orders_dw AS SELECT id, amount, created_at FROM orders;
CREATE INDEX orders_dw_col ON orders_dw USING sift_col (id, amount, created_at) WITH (order_columns = 'created_at');
VACUUM (FREEZE, ANALYZE) orders_dw;
--@ pg
SELECT id, created_at FROM orders_dw WHERE created_at <= timestamp '2024-09-01' ORDER BY created_at DESC LIMIT 10
