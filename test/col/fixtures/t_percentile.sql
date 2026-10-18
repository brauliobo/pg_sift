-- trap: an ordered-set aggregate and other aggregates with their own transition state
-- shape: trap
-- expect: any
-- key: -
SELECT source, percentile_cont(0.5) WITHIN GROUP (ORDER BY store_id) AS median, stddev_samp(store_id) AS sd, bool_and(deleted_at IS NULL) AS all_live,
       bit_or(store_id) AS bits, count(DISTINCT store_id % 10) AS mods, sum(DISTINCT store_id % 10) AS dsum
FROM orders GROUP BY source ORDER BY source
