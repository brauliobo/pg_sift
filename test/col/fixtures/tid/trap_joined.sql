-- trap: ctid of the joined table, not of the scanned one (Format 2.0 reads the joined table as a lookup that tests its own ctid; Format 1 keeps the normal plan)
-- shape: tid
-- expect: any
-- key: -
SELECT count(*) AS n, sum(o.total) AS revenue FROM orders o JOIN tenants s ON s.id = o.tenant_id WHERE s.ctid >= '(0,2)' AND o.tenant_id < 100
