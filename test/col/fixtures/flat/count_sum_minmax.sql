-- flat: a global aggregate over int, date and dictionary columns (one row; sum(int4) is bigint, avg(int4) numeric)
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, count(customer_id) AS customers, sum(tenant_id) AS store_sum, avg(tenant_id) AS store_avg, min(tenant_id) AS store_lo, max(tenant_id) AS store_hi,
       min(placed_at) AS first_day, max(placed_at) AS last_day, min(country_code) AS cc_lo, max(source) AS source_hi
FROM orders
