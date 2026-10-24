-- a range of the key: the units of the range are read
-- label: `sum(amount) WHERE customer_id BETWEEN <a> AND <b>` over 1% of the customers, `key_cut = 'customer_id'`, a btree on the column
--@ setup
CREATE INDEX orders_customer ON orders (customer_id)
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id BETWEEN 1000 AND 1099
