-- an AND of ORs over two integer leaves and an OR of ANDs of them, with a disjunct on a text leaf that no zone can refuse
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT tenant_id % 5 AS k, count(*) AS n, count(*) FILTER (WHERE status = 'paid') AS paid
FROM orders
WHERE ((placed_at < DATE '2022-06-01' OR placed_at > DATE '2025-06-01') AND (tenant_id BETWEEN 10 AND 20 OR tenant_id IN (100, 250, 399)))
   OR (tenant_id = 7 AND placed_at BETWEEN DATE '2023-01-01' AND DATE '2023-03-31')
   OR (tenant_id > 395 AND source = 'web')
GROUP BY 1 ORDER BY 1
