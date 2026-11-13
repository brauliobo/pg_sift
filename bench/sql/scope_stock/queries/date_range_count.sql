-- A date range: the events of the second half of 2025 of the members of one tenant
-- order: 8
--@ events
SELECT count(*) FROM {src} WHERE {pred} AND created_at >= '2025-06-01' AND created_at < '2026-01-01'
--@ rollup
SELECT coalesce(sum(n), 0) FROM r_month WHERE {pred} AND month >= '2025-06-01' AND month < '2026-01-01'
--@ ivm
SELECT coalesce(sum(n), 0) FROM im_main WHERE tenant_id = {t} AND month >= '2025-06-01' AND month < '2026-01-01'
