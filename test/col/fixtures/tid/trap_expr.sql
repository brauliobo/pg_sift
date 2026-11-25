-- trap: ctid inside an expression
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(tenant_id) AS s FROM orders WHERE ctid::text LIKE '(1%'
