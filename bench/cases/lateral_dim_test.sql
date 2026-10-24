-- a LIMIT 1 child that tests its row besides the unique key
-- label: `LEFT JOIN LATERAL (SELECT d.tier FROM dims d WHERE d.id = e.parent_id AND d.active LIMIT 1)`
-- ordered: yes
--@ pg
SELECT l.tier, count(*) AS n, sum(e.amount) AS total FROM events e
LEFT JOIN LATERAL (SELECT d.tier FROM dims d WHERE d.id = e.parent_id AND d.active LIMIT 1) l ON true
WHERE e.deleted_at IS NULL AND e.ts >= date '2024-03-01' AND e.ts < date '2025-03-01' GROUP BY 1 ORDER BY 1
