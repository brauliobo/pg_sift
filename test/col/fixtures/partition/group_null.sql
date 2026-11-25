-- preagg: a dictionary column with NULLs: the NULL group, count of the key itself, sums of two numerics
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, count(status) AS named, sum(discount) AS discounts, avg(total) AS mean
FROM orders
GROUP BY status
ORDER BY status
