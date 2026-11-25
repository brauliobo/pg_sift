-- ordered sets: percentile_cont and percentile_disc per category over the expanded items, ascending and descending, sharing one argument
-- shape: whole-percentiles-by-category
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price) AS p50, percentile_cont(0.95) WITHIN GROUP (ORDER BY i.price) AS p95,
       percentile_cont(0.3) WITHIN GROUP (ORDER BY i.price DESC) AS d30, percentile_disc(0.5) WITHIN GROUP (ORDER BY i.price) AS q50,
       percentile_disc(0) WITHIN GROUP (ORDER BY i.price DESC) AS q0, percentile_disc(1) WITHIN GROUP (ORDER BY i.price) AS q100,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price) FILTER (WHERE i.qty > 2) AS p50_big,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY i.qty * (i.price - i.discount)) AS net50
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
WHERE o.deleted_at IS NULL
GROUP BY i.category
ORDER BY i.category
