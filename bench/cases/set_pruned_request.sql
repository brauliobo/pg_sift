-- a request for a few customers out of 200,000: the units whose key list lacks them are dropped by their small-set summaries, which are checked in one loop for each width of the list
-- label: `count(*)`, `sum(amount)` of the orders of three customers, `key_columns = 'customer_id'`
-- ordered: yes
--@ setup
DROP INDEX orders_col;
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at) WITH (key_columns = 'customer_id')
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id IN (11, 29, 4711)
