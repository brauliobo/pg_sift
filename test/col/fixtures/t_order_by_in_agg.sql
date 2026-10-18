-- trap: ORDER BY inside aggregates (string_agg, array_agg)
-- shape: trap
-- expect: plain
-- key: -
SELECT source, string_agg(country_code, ',' ORDER BY country_code, id) AS ccs, array_agg(id ORDER BY tenant_id, id) AS ids
FROM orders WHERE tenant_id < 4 GROUP BY source ORDER BY source
