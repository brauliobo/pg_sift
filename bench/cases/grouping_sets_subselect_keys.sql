-- GROUPING SETS over keys that are expressions of a sub-select: the planner hands them to the grouping step as placeholders
-- label: `GROUPING SETS ((country_u), (country_u, year), ())` over `upper(country)` and `extract(year FROM created_at)` of a sub-select, `count`, `sum`
-- ordered: yes
--@ pg
SELECT country_u, yr, count(*) AS n, sum(amount) AS revenue FROM (SELECT upper(country) AS country_u, extract(year FROM created_at)::int AS yr, amount FROM orders) p
GROUP BY GROUPING SETS ((country_u), (country_u, yr), ()) ORDER BY country_u NULLS FIRST, yr NULLS FIRST
