-- flat: HAVING on aggregates that are not in the select list
-- shape: flat
-- expect: agg
-- key: -
SELECT store_id, count(*) AS n, max(placed_at) AS last_day
FROM orders
GROUP BY store_id
HAVING count(*) > 8 AND max(placed_at) > DATE '2025-06-01' AND min(store_id) > 0
ORDER BY n DESC, store_id
