-- ORDER BY in the call under grouping sets, an expression of two columns as the key, a global aggregate and a group without rows
-- shape: S3
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, tenant_id % 3 AS g, grouping(source, tenant_id % 3) AS gr, array_agg(id ORDER BY tenant_id * 7 % 11, id) AS ids,
       string_agg(DISTINCT lang, ',') AS langs, max(id) AS top
FROM orders WHERE tenant_id < 12 GROUP BY ROLLUP (source, tenant_id % 3) ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
