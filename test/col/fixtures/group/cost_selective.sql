-- generic aggregates over a few dozen rows keep the btree plan: the scan's start-up and its per-group price are not paid for them
-- shape: trap
-- expect: plain
-- key: -
SELECT source, stddev_samp(tenant_id) AS sd, bool_and(total > 10) AS big, min(country_code) AS lo, array_agg(DISTINCT status) AS statuses
FROM orders WHERE id < 40 GROUP BY source ORDER BY source
