-- Events since 2025 counted by kind, of the members of one tenant
-- order: 9
--@ events
SELECT kind, count(*) FROM {src} WHERE {pred} AND created_at >= '2025-01-01' GROUP BY 1
--@ rollup
SELECT kind, sum(n) FROM r_kind WHERE {pred} AND month >= '2025-01-01' GROUP BY 1
--@ ivm
SELECT kind, sum(n) FROM im_kind WHERE tenant_id = {t} AND month >= '2025-01-01' GROUP BY 1
