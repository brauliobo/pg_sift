-- a join to a sub-select with GROUP BY: an aggregate of the table of stores per country, one row per key
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT c.stores, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN (SELECT country_code, count(*) AS stores FROM stores GROUP BY country_code) c ON o.country_code = c.country_code
WHERE o.deleted_at IS NULL
GROUP BY c.stores ORDER BY c.stores
