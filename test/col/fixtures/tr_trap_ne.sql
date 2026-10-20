-- trap: <> on ctid is not a range
-- shape: tid
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(tenant_id) AS s FROM orders WHERE ctid <> '(3,2)'
