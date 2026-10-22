-- NULL-heavy documents: items without a kind (JSON null or no key), orders without any metric, an empty notes array
-- shape: nulls
-- expect: any
-- key: -
-- scenario: yes
SELECT i.kind, count(*) AS items, count(DISTINCT o.id) AS orders, count(*) FILTER (WHERE o.metrics->>'p_total' IS NULL) AS no_p_total,
       count(*) FILTER (WHERE jsonb_array_length(o.notes) = 0) AS no_notes
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(kind text)
WHERE o.deleted_at IS NULL
GROUP BY i.kind
ORDER BY i.kind NULLS FIRST
