-- source filters: a text equality that no row of the dictionary has, and one on a column with NULLs
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT (SELECT count(*) FROM orders WHERE status = 'unknown') AS none,
       (SELECT count(*) FROM orders WHERE status <> 'paid') AS ne_paid,
       (SELECT count(*) FROM orders WHERE country_code IN ('DE', 'FR', 'ES') AND status IS NOT NULL) AS in_notnull,
       (SELECT sum(store_id) FROM orders WHERE status NOT IN ('paid', 'shipped', 'delivered')) AS notin,
       (SELECT count(*) FROM orders WHERE status = 'shipped' AND country_code = 'DE' AND placed_at >= DATE '2025-01-01' AND placed_at < DATE '2026-01-01') AS q05
