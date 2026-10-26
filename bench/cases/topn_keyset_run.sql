-- the page after a key, ORDER BY a microsecond timestamp that arrives in no order and the id, LIMIT 50, no b-tree on it: the units are read through the sorted run from the range of the key, and the page's condition is tested on the rows the walk takes, where every row of every unit was tested
-- label: `SELECT id, created_at WHERE (created_at, id) > ('2024-09-01', 500000) ORDER BY created_at, id LIMIT 50`, `order_columns = 'created_at'`, no b-tree; the variant `no_run` is the same index without the sorted runs
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS topn_keyset_run_plain CASCADE;
CREATE SCHEMA topn_keyset_run_plain;
CREATE TABLE orders_kr AS SELECT id, amount, created_at + (hashint8extended(id, 7) & 1048575) * interval '1 microsecond' AS created_at FROM orders;
CREATE INDEX orders_kr_col ON orders_kr USING sift_col (id, amount, created_at) WITH (order_columns = 'created_at');
CREATE TABLE topn_keyset_run_plain.orders_kr AS TABLE orders_kr;
CREATE INDEX orders_kr_col ON topn_keyset_run_plain.orders_kr USING sift_col (id, amount, created_at);
VACUUM (FREEZE, ANALYZE) orders_kr;
VACUUM (FREEZE, ANALYZE) topn_keyset_run_plain.orders_kr;
--@ pg
SELECT id, created_at FROM orders_kr WHERE (created_at, id) > (timestamp '2024-09-01', 500000) ORDER BY created_at, id LIMIT 50
