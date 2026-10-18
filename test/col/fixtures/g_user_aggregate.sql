-- a user aggregate with an immutable transition function (SQL language) and an initial value runs on the generic path
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT tenant_id % 3 AS g, coldiff.sum_sq(total::float8) AS sq, coldiff.sum_sq(tenant_id) FILTER (WHERE status IS NULL) AS sq_null
FROM orders GROUP BY tenant_id % 3 ORDER BY 1
