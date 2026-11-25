-- no group by: one row, also when nothing matches
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(tenant_id) AS s, min(id) AS lo, max(id) AS hi FROM orders WHERE memo @@> 'perfect thanks'
