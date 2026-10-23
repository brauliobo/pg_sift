-- the elements of a jsonb array leaf counted with a filter on them (the decoded list lengths are kept for the backend)
-- label: `jsonb_to_recordset(doc -> 'parts')` with `score > 5`, `count` by `kind`
-- ordered: yes
--@ pg
SELECT p.kind, count(*) AS n FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> 'parts') p(kind text, score float8)
WHERE p.score > 5 AND e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1
