-- Events per month since 2023 with their amount, of the members one tenant reaches
-- order: 1
--@ events
SELECT date_trunc('month', created_at) m, count(*), sum(amount) FROM {src} WHERE {pred} AND created_at >= '2023-01-01' GROUP BY 1
--@ rollup
SELECT month::timestamptz m, sum(n), sum(amount_sum) FROM r_month WHERE {pred} AND month >= '2023-01-01' GROUP BY month
--@ ivm
SELECT month::timestamptz m, sum(n), sum(amount_sum) FROM im_main WHERE tenant_id = {t} AND month >= '2023-01-01' GROUP BY month
