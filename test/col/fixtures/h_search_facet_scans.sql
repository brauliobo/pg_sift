-- the statement of sift.facet_sql: a GROUP BY scan per facet (a column, a month) and the ranges of a column as FILTER aggregates of one scan, over the same match set
-- shape: search
-- expect: agg
-- key: -
-- scenario: yes
WITH s1 AS MATERIALIZED (SELECT (store_id) k1, count(*) c FROM orders WHERE (memo) @@> 'late OR missing OR refund' AND (status <> 'cancelled') GROUP BY (store_id)),
     s2 AS MATERIALIZED (SELECT date_trunc('month', (placed_at)::timestamp) k2, count(*) c FROM orders WHERE (memo) @@> 'late OR missing OR refund' AND (status <> 'cancelled') GROUP BY date_trunc('month', (placed_at)::timestamp)),
     r1 AS MATERIALIZED (SELECT count(*) FILTER (WHERE (total) < 100) a3_0, count(*) FILTER (WHERE (total) >= 100 AND (total) < 300) a3_1, count(*) FILTER (WHERE (total) >= 300) a3_2,
                                count(*) FILTER (WHERE (total) IS NULL) a3_m FROM orders WHERE (memo) @@> 'late OR missing OR refund' AND (status <> 'cancelled'))
SELECT 1 AS facet, k1::text AS k, c FROM s1 UNION ALL SELECT 2, k2::text, c FROM s2
UNION ALL SELECT 3, 'lt100', a3_0 FROM r1 UNION ALL SELECT 3, '100-300', a3_1 FROM r1 UNION ALL SELECT 3, 'ge300', a3_2 FROM r1 UNION ALL SELECT 3, 'null', a3_m FROM r1
ORDER BY 1, 2
