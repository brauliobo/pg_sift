-- the same aggregates over an inner lookup join with a rollup and a FILTER: a group with only NULL values is NULL, a group without a row has no row
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.category, i.kind, GROUPING(m.category, i.kind) AS g, sum(m.i2) AS s2, avg(m.i4) AS a4, sum(m.i8) AS s8, avg(m.iv) AS ai, sum(m.i4) FILTER (WHERE i.qty > 1) AS sq, avg(m.f4) AS af, count(*) AS n
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN map_nums m ON m.category = i.category
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY ROLLUP (m.category, i.kind) ORDER BY g, m.category NULLS FIRST, i.kind NULLS FIRST
