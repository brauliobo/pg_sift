-- the same lookup when the column has a btree: the loss to register
-- label: `count(*), sum(amount) WHERE customer_id = <one customer>`, `key_cut = 'customer_id'`, a btree on the column
--@ setup
CREATE INDEX orders_customer ON orders (customer_id)
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id = 137
