-- the three best categories of every country by revenue: a window function over the groups of an aggregate, filtered by its rank
-- label: `row_number() OVER (PARTITION BY country ORDER BY sum(amount) DESC)` over `GROUP BY country, category`, the first 3 of each country
-- ordered: yes
--@ pg
SELECT country, category, revenue FROM (
  SELECT country, category, sum(amount) AS revenue, row_number() OVER (PARTITION BY country ORDER BY sum(amount) DESC, category) AS rn FROM orders GROUP BY country, category) t
WHERE rn <= 3 ORDER BY country, rn
