-- the groups of a key that is itself a COALESCE of two columns, under a window on it: the scan gives them in the window's order, no Sort above it
-- label: `lag(n) OVER (PARTITION BY k ORDER BY customer_id DESC)` over `count(*) GROUP BY coalesce(status, country), customer_id`, about 100,000 groups
-- ordered: yes
--@ pg
SELECT k, customer_id, n, lag(n) OVER (PARTITION BY k ORDER BY customer_id DESC) AS previous FROM (SELECT coalesce(status, country) AS k, customer_id, count(*) AS n FROM orders GROUP BY coalesce(status, country), customer_id) g ORDER BY k, customer_id DESC
