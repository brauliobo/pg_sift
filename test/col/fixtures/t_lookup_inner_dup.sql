-- trap: an inner join drops the unmatched items and replicates the ones with several rows
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, m.tag, count(*) AS n, sum(i.qty) AS qty
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN map_dup m ON m.category = i.category
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category, m.tag ORDER BY 1, 2
