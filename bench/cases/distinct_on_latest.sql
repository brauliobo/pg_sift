-- the latest order of each customer of a country: DISTINCT ON keeps the first row of each customer in the order of the statement, against a b-tree whose order is that of the statement
-- label: `SELECT DISTINCT ON (customer_id) .. WHERE country = 'US' ORDER BY customer_id, created_at DESC, id DESC LIMIT 200`; the variant `btree` is a copy of the orders with a b-tree on (country, customer_id, created_at DESC, id DESC)
-- ordered: yes
--@ setup
DROP SCHEMA IF EXISTS c_distinct_on_latest__btree CASCADE;
CREATE SCHEMA c_distinct_on_latest__btree;
CREATE TABLE c_distinct_on_latest__btree.orders AS SELECT * FROM orders ORDER BY id;
CREATE INDEX orders_latest ON c_distinct_on_latest__btree.orders (country, customer_id, created_at DESC, id DESC);
VACUUM (FREEZE, ANALYZE) c_distinct_on_latest__btree.orders
--@ pg
SELECT DISTINCT ON (customer_id) customer_id, created_at, amount FROM orders WHERE country = 'US' ORDER BY customer_id, created_at DESC, id DESC LIMIT 200
