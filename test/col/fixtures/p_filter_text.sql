-- source filters: =, <>, IN and NOT IN of a text column (NULL statuses and countries, a member the dictionaries lack), a selective one with aggregates of other columns
-- shape: quals
-- expect: agg
-- key: -
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- guc: enable_seqscan = off
SELECT lang, count(*) AS n, count(status) AS st, sum(tenant_id) AS s, avg(total) AS t, min(placed_at) AS lo, max(id) AS hi,
       count(*) FILTER (WHERE country_code = 'ZZ') AS none_eq
FROM orders
WHERE status = 'shipped' AND country_code <> 'US' AND source IN ('web', 'pos', 'nowhere') AND lang NOT IN ('en', 'xx')
GROUP BY lang
ORDER BY lang
