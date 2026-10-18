-- NULL and empty sets: a grouped aggregate over no row is no row
-- shape: empty
-- expect: agg
-- key: -
SELECT source, count(*) AS n, sum(store_id) AS s FROM orders WHERE placed_at > DATE '2100-01-01' GROUP BY source ORDER BY source
