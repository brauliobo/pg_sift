-- a lookup of the rows of one foreign key with no btree on it: the aggregate of one customer
-- label: `count(*), sum(amount) WHERE customer_id = <one customer>`, no btree on `customer_id`, `point_tid_columns = 'customer_id'`
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id = 137
