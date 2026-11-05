-- the groups read by a query that groups them again with an aggregate that sorts (its own ORDER BY): the scan gives them in that order, no Sort above it
-- label: `array_agg(n ORDER BY customer_id) GROUP BY country` over `count(*) GROUP BY country, customer_id`, about 100,000 groups
-- ordered: yes
--@ pg
SELECT country, array_agg(n ORDER BY customer_id) AS per_customer FROM (SELECT country, customer_id, count(*) AS n FROM orders GROUP BY country, customer_id) g GROUP BY country ORDER BY country
