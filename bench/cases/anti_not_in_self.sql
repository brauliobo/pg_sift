-- an anti join as NOT IN (SELECT ..) on the same table: the orders of the customers that never cancelled
-- label: `WHERE customer_id NOT IN (SELECT customer_id FROM orders WHERE status = 'cancelled')`, `count`, `sum`
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id NOT IN (SELECT customer_id FROM orders WHERE status = 'cancelled')
