-- the groups of an aggregate with three keys under a window on two of them, in the window's order from the scan: no Sort above it
-- label: `lag(n) OVER (PARTITION BY country, customer_id ORDER BY segment_id DESC)` over `count(*) GROUP BY country, customer_id, segment_id`, about 200,000 groups
-- ordered: yes
--@ pg
SELECT country, customer_id, segment_id, n, lag(n) OVER (PARTITION BY country, customer_id ORDER BY segment_id DESC) AS previous FROM (SELECT country, customer_id, segment_id, count(*) AS n FROM orders GROUP BY country, customer_id, segment_id) g ORDER BY country, customer_id, segment_id DESC
