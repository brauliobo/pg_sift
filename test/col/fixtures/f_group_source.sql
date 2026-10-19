-- flat: count, distinct and min/max per dictionary column
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, count(DISTINCT store_id) AS stores, count(DISTINCT country_code) AS countries, min(placed_at) AS first_day, max(placed_at) AS last_day
FROM orders
GROUP BY source
ORDER BY source
