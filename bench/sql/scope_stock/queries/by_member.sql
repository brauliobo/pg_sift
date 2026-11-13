-- Events and amount of every member of one tenant
-- order: 6
--@ events
SELECT dim_a_id, count(*), sum(amount) FROM {src} WHERE {pred} GROUP BY 1
--@ rollup
SELECT dim_a_id, sum(n), sum(amount_sum) FROM r_month WHERE {pred} GROUP BY 1
--@ ivm
SELECT dim_a_id, sum(n), sum(amount_sum) FROM im_main WHERE tenant_id = {t} GROUP BY 1
