-- a semi join on the same table: the orders of the customers that have a cancelled order
-- label: `WHERE EXISTS (SELECT 1 FROM orders o2 WHERE o2.customer_id = o.customer_id AND o2.status = 'cancelled')`, `count`, `sum`
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders o WHERE EXISTS (SELECT 1 FROM orders o2 WHERE o2.customer_id = o.customer_id AND o2.status = 'cancelled')
