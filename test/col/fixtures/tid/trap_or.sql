-- trap: a range of ctid under an OR
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(tenant_id) AS s FROM orders WHERE ctid < '(10,1)' OR tenant_id = 3
