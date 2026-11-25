-- ctid: equality with a tid that no row has (a slot past the last of its page)
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT count(*) AS n, sum(tenant_id) AS s FROM orders WHERE ctid = '(3,200)'
