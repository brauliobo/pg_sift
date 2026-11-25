-- trap: ctid of the joined table, not of the scanned one
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(o.total) AS revenue FROM orders o JOIN tenants s ON s.id = o.tenant_id WHERE s.ctid >= '(0,2)' AND o.tenant_id < 100
