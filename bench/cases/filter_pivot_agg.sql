-- a pivot of one pass: counts and sums under FILTER clauses of the status and the amount, one column per filter, grouped by the country
-- label: `count(*) FILTER (WHERE status = 'shipped')`, `sum(amount) FILTER (WHERE status = 'shipped')`, `count(*) FILTER (WHERE status = 'cancelled')`, `sum(amount) FILTER (WHERE amount > 200)` by country
-- ordered: yes
--@ pg
SELECT country, count(*) FILTER (WHERE status = 'shipped') AS shipped_n, sum(amount) FILTER (WHERE status = 'shipped') AS shipped_rev,
  count(*) FILTER (WHERE status = 'cancelled') AS cancel_n, sum(amount) FILTER (WHERE amount > 200) AS big_rev FROM orders GROUP BY 1 ORDER BY 1
