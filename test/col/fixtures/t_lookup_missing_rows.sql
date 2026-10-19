-- trap: the keys without a row in the mapping table are NULL in a left join and gone in an inner join
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT coalesce(m.tag, '-') AS tag, count(*) AS n, count(DISTINCT i.category) AS categories
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_part m ON m.category = i.category
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY 1 ORDER BY 1
