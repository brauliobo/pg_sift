-- ctid: the constant on the left
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(tenant_id) AS s FROM orders WHERE '(20,1)' <= ctid AND '(90,1)' > ctid
