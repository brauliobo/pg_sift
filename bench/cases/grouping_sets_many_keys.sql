-- GROUPING SETS over keys of thousands of values (the groups are kept in a hash table): the key ids of a chunk are prepared once for all the sets that read its rows
-- label: `GROUPING SETS ((customer_id, country), (customer_id), (country), ())`, `count`, `sum`
-- ordered: yes
--@ pg
SELECT customer_id, country, count(*) AS n, sum(amount) AS revenue FROM orders
GROUP BY GROUPING SETS ((customer_id, country), (customer_id), (country), ()) ORDER BY customer_id NULLS FIRST, country NULLS FIRST
