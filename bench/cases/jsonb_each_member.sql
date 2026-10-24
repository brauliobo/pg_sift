-- one member of a JSON object read through jsonb_each: a LEFT JOIN LATERAL that keeps the rows without it
-- label: `LEFT JOIN LATERAL jsonb_each(doc -> 'metrics') r ON r.key = 'm_a'`, `count`, `avg(CAST(r.value AS float))` by `kind`
-- ordered: yes
--@ pg
SELECT e.kind, count(*) AS n, count(CAST(r.value AS float)) AS members, avg(CAST(r.value AS float)) AS avg_value FROM events e
LEFT JOIN LATERAL jsonb_each(e.doc -> 'metrics') r ON r.key = 'm_a' WHERE e.deleted_at IS NULL AND e.ts >= date '2025-01-01' GROUP BY 1 ORDER BY 1
