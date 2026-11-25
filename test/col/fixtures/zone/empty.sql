-- zone pruning: contradictory bounds select no unit and no row, and an equality selects the one unit that can hold it
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT count(*) AS n, sum(tenant_id) AS s, min(id) AS lo, count(*) FILTER (WHERE id = 777) AS one
FROM orders
WHERE id > 500 AND id < 400
