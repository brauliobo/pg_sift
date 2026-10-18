-- having and order by an aggregate on a match set
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT tenant_id, count(*) AS n FROM orders WHERE memo @@> 'late OR missing' GROUP BY tenant_id HAVING count(*) > 2 ORDER BY n DESC, tenant_id LIMIT 20
