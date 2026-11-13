-- Members with an event, per month since 2022, of one tenant
-- order: 5
--@ events
SELECT date_trunc('month', created_at) m, count(DISTINCT dim_a_id) FROM {src} WHERE {pred} AND created_at >= '2022-01-01' GROUP BY 1
--@ rollup
SELECT month::timestamptz m, count(DISTINCT dim_a_id) FROM r_month WHERE {pred} AND month >= '2022-01-01' GROUP BY month
--@ ivm
SELECT month::timestamptz m, count(DISTINCT dim_a_id) FROM im_main WHERE tenant_id = {t} AND month >= '2022-01-01' GROUP BY month
