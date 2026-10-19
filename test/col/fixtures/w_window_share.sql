-- window: the share of the total of each group, over the whole result
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT country_code, count(*) AS n, round(100.0 * count(*) / sum(count(*)) OVER (), 3) AS pct, count(*) OVER () AS groups
FROM orders
GROUP BY country_code
ORDER BY n DESC, country_code
