-- a lookup of one customer for two ranges of dates (an OR of ranges is a tree of zones) on an index that cuts its units by the customer: the scan is set up once, not once for each morsel of units
-- label: `count(*), sum(amount) WHERE customer_id = <one customer> AND (order_date in January OR order_date in June 2025)`, `key_cut = 'customer_id'`, no btree on the column
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE customer_id = 137
AND ((order_date >= DATE '2025-01-01' AND order_date < DATE '2025-02-01') OR (order_date >= DATE '2025-06-01' AND order_date < DATE '2025-07-01'))
