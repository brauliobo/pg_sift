-- preagg: rollup: aggregates with FILTER on dims (the shares of a rating band) next to unfiltered ones, grouped by a dim
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, count(*) FILTER (WHERE i.kind = 'gift') AS gifts, count(*) FILTER (WHERE i.kind <> 'gift' OR i.kind IS NULL) AS others,
       sum(i.qty) FILTER (WHERE i.category <> 'Home') AS qty, avg(i.price) FILTER (WHERE i.kind = 'standard') AS mean_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
GROUP BY i.category
ORDER BY i.category
