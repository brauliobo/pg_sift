-- generic aggregates of expressions of several leaves, of the rows and of the unnested items, with FILTER; the expressions are PostgreSQL's own
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, stddev_samp(i.qty * (i.price - i.discount)) AS sd, corr(i.price, i.qty) AS c, max(upper(i.category) || o.country_code) AS mx,
       min(o.total * 2 - o.discount) AS lo, variance(o.total) FILTER (WHERE i.qty > 2) AS vf, bool_and(i.price > o.total / 100) AS cheap
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
WHERE o.deleted_at IS NULL
GROUP BY i.category
ORDER BY i.category
