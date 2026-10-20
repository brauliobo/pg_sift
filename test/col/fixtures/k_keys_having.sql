-- six group columns with a HAVING on the count and on an aggregate that is not selected
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.source, o.status, o.lang, o.country_code, o.tenant_id % 5 AS st5, o.placed_at > date '2023-06-30' AS late,
       count(*) AS n, avg(o.total) AS avg_total, max(o.id) AS last_id
FROM orders o
WHERE o.tenant_id < 300
GROUP BY 1, 2, 3, 4, 5, 6
HAVING count(*) > 1 AND min(o.id) > 10
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST, 5, 6
