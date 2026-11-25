-- preagg: a global aggregate of counts, sums, averages and the zone's min and max
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, count(customer_id) AS customers, count(status) AS named, sum(total) AS revenue, avg(discount) AS mean_discount,
       min(tenant_id) AS store_lo, max(tenant_id) AS store_hi, min(placed_at) AS first_day, max(placed_at) AS last_day, sum(tenant_id) AS store_sum
FROM orders
