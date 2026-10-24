-- the rows of a top-N scan come from the units that gave them when every output column is a leaf of the index, where each was fetched from the heap
-- label: `SELECT created_at, customer_id ORDER BY created_at DESC, customer_id DESC LIMIT 5000` after `CLUSTER` on `created_at`, no btree on it
-- ordered: yes
--@ setup
CLUSTER orders USING orders_created;
DROP INDEX orders_created;
ANALYZE orders
--@ pg
SELECT created_at, customer_id FROM orders ORDER BY created_at DESC, customer_id DESC LIMIT 5000
