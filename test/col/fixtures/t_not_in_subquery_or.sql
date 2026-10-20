-- NOT IN under OR and a negated IN, with a NULL in the sub-select (NULL, so the OR decides) and a NOT IN of the dictionary column
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.source, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL
  AND (o.customer_id NOT IN (SELECT m.rank FROM map_vals m) OR NOT (o.status IN (SELECT k.status FROM dim_status k WHERE k.stage > 1)) OR o.store_id < 20)
GROUP BY o.source ORDER BY o.source
