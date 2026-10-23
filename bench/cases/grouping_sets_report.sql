-- a report with four levels in one statement: GROUPING SETS over two dimensions, a date filter and three aggregates
-- label: `GROUPING SETS ((country, category), (country), (category), ())`, `count`, `sum`, `avg`, `WHERE created_at >= '2025-01-01'`
-- ordered: yes
--@ pg
SELECT country, category, count(*) AS n, sum(amount) AS revenue, avg(amount) AS avg_amount FROM orders WHERE created_at >= TIMESTAMP '2025-01-01'
GROUP BY GROUPING SETS ((country, category), (country), (category), ()) ORDER BY country NULLS FIRST, category NULLS FIRST
