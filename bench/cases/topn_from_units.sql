-- a top-N scan whose output columns are all leaves of the index: the order array answers it (`Global Order` in EXPLAIN) and the rows are fetched from the heap in the order of the array
-- label: `SELECT created_at, customer_id ORDER BY created_at DESC, customer_id DESC LIMIT 5000` after `CLUSTER` on `created_at`, no btree on it
-- ordered: yes
--@ setup
CLUSTER orders USING orders_created;
DROP INDEX orders_created;
ANALYZE orders
--@ pg
SELECT created_at, customer_id FROM orders ORDER BY created_at DESC, customer_id DESC LIMIT 5000
