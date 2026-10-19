-- the statement of sift.facet_sql: the facets of a search as GROUPING SETS of four keys (a column, a column, the buckets of a range as a CASE, a month) over one match set
-- shape: search
-- expect: agg
-- key: -
-- scenario: yes
SELECT GROUPING(store_id) g1, store_id k1, GROUPING(status) g2, status k2,
       GROUPING(CASE WHEN total IS NULL THEN NULL WHEN total < 100 THEN 0 WHEN total < 300 THEN 1 ELSE 2 END) g3,
       CASE WHEN total IS NULL THEN NULL WHEN total < 100 THEN 0 WHEN total < 300 THEN 1 ELSE 2 END k3,
       GROUPING(date_trunc('month', placed_at::timestamp)) g4, date_trunc('month', placed_at::timestamp) k4, count(*) c
FROM orders WHERE memo @@> 'late OR missing OR refund' AND status <> 'cancelled'
GROUP BY GROUPING SETS ((store_id), (status), (CASE WHEN total IS NULL THEN NULL WHEN total < 100 THEN 0 WHEN total < 300 THEN 1 ELSE 2 END), (date_trunc('month', placed_at::timestamp)))
ORDER BY 1, 3, 5, 7, 2, 4, 6, 8
