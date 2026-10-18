-- PostgreSQL-evaluated numeric expressions over the orders (many distinct totals): a quotient and a ratio as arguments, a modulo and a rounded quotient as keys, a float8 comparison and a guarded division as filters
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT store_id % 5 AS k, round(total / 100, 0) AS r, count(*) AS n, sum(total / 3) AS s, avg(total / (discount + 1)) AS a, max(sqrt(abs(total))) AS q,
	   count(*) FILTER (WHERE total > 100.5::float8 AND discount <> 0 AND total / discount > 2) AS f
FROM orders WHERE total IS NOT NULL GROUP BY 1, 2 ORDER BY 1, 2 NULLS FIRST
