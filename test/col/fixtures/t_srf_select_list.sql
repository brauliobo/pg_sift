-- trap: a set-returning function in the select list of an aggregate query
-- shape: trap
-- expect: plain
-- key: -
SELECT source, count(*) AS n, generate_series(1, 2) AS g FROM orders WHERE tenant_id < 30 GROUP BY source ORDER BY source, g
