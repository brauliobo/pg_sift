-- hypothetical-set aggregates: the rows of a group go through the aggregate's own tuplesort (NULLs count), the hypothetical values are evaluated once
-- shape: whole-percentiles-by-category
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, rank(10) WITHIN GROUP (ORDER BY tenant_id) AS r, dense_rank(10) WITHIN GROUP (ORDER BY tenant_id) AS dr,
       percent_rank(10, 'paid') WITHIN GROUP (ORDER BY tenant_id, status) AS pr, cume_dist(5) WITHIN GROUP (ORDER BY tenant_id DESC) AS cd,
       rank(NULL, 'x') WITHIN GROUP (ORDER BY country_code NULLS FIRST, status) FILTER (WHERE tenant_id < 50) AS rn,
       rank(1) WITHIN GROUP (ORDER BY id) FILTER (WHERE id < 0) AS none
FROM orders GROUP BY source ORDER BY source
