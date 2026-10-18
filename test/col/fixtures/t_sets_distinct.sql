-- trap: GROUP BY DISTINCT removes the repeated sets that the plain form keeps (their rows come twice)
-- shape: trap
-- expect: agg
-- key: -
SELECT o.lang, o.source, grouping(o.lang, o.source) AS g, count(*) AS n
FROM orders o WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY DISTINCT ROLLUP (o.lang, o.source), ROLLUP (o.lang) ORDER BY 3, 1 NULLS FIRST, 2 NULLS FIRST
