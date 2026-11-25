-- the rest of an ON clause: a condition on the table's row, on the table's row and the item; a row of the item without a passing match is the LEFT JOIN's NULL row
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, m.label, count(*) AS n, count(m.rank) AS ranked, sum(m.weight) AS weight, max(m.since) AS since
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_vals m ON m.category = i.category AND m.rank > 1 AND (m.weight IS NULL OR m.weight * i.qty > 1)
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category, m.label ORDER BY i.category NULLS FIRST, m.label NULLS FIRST
