-- NULL-heavy columns: counts of the NULLs, aggregates that skip them and the group of the NULLs
-- shape: nulls
-- expect: agg
-- key: -
SELECT country_code, count(*) AS n, count(country_code) AS c, count(customer_id) AS customers, count(deleted_at) AS deleted, sum(customer_id) AS customer_sum,
       min(status) AS lo, max(status) AS hi
FROM orders
WHERE tenant_id < 60
GROUP BY country_code
ORDER BY country_code NULLS FIRST
