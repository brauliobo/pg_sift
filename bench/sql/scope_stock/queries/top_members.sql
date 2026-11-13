-- The 20 members with the most events among those of one tenant
-- order: 4
--@ events
SELECT dim_a_id, count(*) c FROM {src} WHERE {pred} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20
--@ rollup
SELECT dim_a_id, sum(n) c FROM r_month WHERE {pred} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20
--@ ivm
SELECT dim_a_id, sum(n) c FROM im_main WHERE tenant_id = {t} GROUP BY 1 ORDER BY c DESC, 1 LIMIT 20
