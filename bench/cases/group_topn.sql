-- the groups of an ORDER BY aggregate LIMIT n that cannot be among the first n are dropped before they become tuples, where PostgreSQL sorted every group
-- label: `GROUP BY customer_id ORDER BY sum(amount) DESC LIMIT 10` (200,000 groups at 1,000,000 orders)
-- ordered: yes
--@ pg
SELECT customer_id, sum(amount) AS value FROM orders GROUP BY customer_id ORDER BY value DESC, customer_id LIMIT 10
