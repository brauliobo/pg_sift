-- a score distribution over cells: eleven outer aggregates, five of them with a FILTER on the cell's average, over the (customer, month) cells of a category
-- label: `avg`, `sum` and `count(*) FILTER (WHERE cell_avg >= 8)` of the average per (customer, month) cell, grouped by category
-- ordered: yes
--@ pg
SELECT category, count(*) AS cells, avg(a) AS mean, sum(n) AS orders, count(*) FILTER (WHERE a >= 8) AS high, count(*) FILTER (WHERE a < 6) AS low, count(*) FILTER (WHERE a > 6 AND a < 8) AS mid, count(*) FILTER (WHERE a <= 6) AS notmid, sum(n) FILTER (WHERE a >= 8) AS high_orders
FROM (SELECT category, customer_id, date_trunc('month', created_at) AS m, avg(amount) AS a, count(*) AS n FROM orders GROUP BY 1, 2, 3) c GROUP BY 1 ORDER BY 1
