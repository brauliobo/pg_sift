-- trap: ROLLUP over a column with NULL values: the NULL group of the data and the NULL of a rollup row are different rows, told apart by GROUPING()
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.kind, i.category, grouping(i.kind) AS g_kind, grouping(i.category, i.kind) AS g_both, count(*) AS n, sum(i.qty) AS qty, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY ROLLUP (i.kind, i.category) ORDER BY 4, 3, 1 NULLS FIRST, 2 NULLS FIRST
