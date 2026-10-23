-- a LIMIT 1 child with an ORDER BY, read as a first-row lookup when a unique index holds the key and the order columns
-- label: `LEFT JOIN LATERAL (.. WHERE f.parent_id = e.parent_id ORDER BY f.ts, f.id LIMIT 1)`, unique index on `(parent_id, ts, id)`
-- ordered: yes
--@ setup
CREATE UNIQUE INDEX IF NOT EXISTS facts_uq ON facts (parent_id, ts, id)
--@ pg
SELECT e.kind, count(*) AS n, sum(l.val) AS total FROM events e
LEFT JOIN LATERAL (SELECT f.val FROM facts f WHERE f.parent_id = e.parent_id ORDER BY f.ts, f.id LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1
