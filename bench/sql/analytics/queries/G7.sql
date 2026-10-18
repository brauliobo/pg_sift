-- G7: whole table, median and 95th percentile of the price per category
-- sets: -
-- key: 1
--@ pg
SELECT i.category, percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price) AS p50, percentile_cont(0.95) WITHIN GROUP (ORDER BY i.price) AS p95
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8)
GROUP BY 1
--@ flat
SELECT i.category, percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price) AS p50, percentile_cont(0.95) WITHIN GROUP (ORDER BY i.price) AS p95
FROM pg_items i GROUP BY 1
--@ ch
SELECT i.category AS category, quantileExactInclusive(0.5)(i.price) AS p50, quantileExactInclusive(0.95)(i.price) AS p95
FROM items_tuned i GROUP BY category
