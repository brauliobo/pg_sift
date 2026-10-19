-- trap: a user aggregate whose transition function is volatile keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
SELECT tenant_id % 3 AS g, round(coldiff.sum_vol(total::float8)::numeric, 0) AS v FROM orders GROUP BY tenant_id % 3 ORDER BY 1
