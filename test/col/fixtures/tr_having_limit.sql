-- ctid: HAVING and LIMIT over a range
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT tenant_id, count(*) AS n FROM orders WHERE ctid >= '(30,1)' AND ctid < '(520,1)' GROUP BY tenant_id HAVING count(*) > 5 ORDER BY n DESC, tenant_id LIMIT 10
