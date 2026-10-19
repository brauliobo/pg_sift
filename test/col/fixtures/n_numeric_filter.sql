-- numeric sums of the orders: totals per store set, a FILTER on another column, no group and an empty set
-- shape: S6
-- expect: agg
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
SELECT country_code, count(*) AS orders, sum(total) AS revenue, avg(total) AS avg_total, sum(total) FILTER (WHERE lang = 'en') AS en_total, max(discount) AS top_discount,
       min(total) FILTER (WHERE store_id > 5) AS lo
FROM orders
WHERE store_id IN ({stores}) AND source IN ({sources}) AND deleted_at IS NULL AND {range(placed_at)}
GROUP BY country_code ORDER BY country_code NULLS FIRST
