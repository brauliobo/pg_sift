-- coalesce of an int column under a filter, then a second key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, customer_id, tenant_id FROM orders WHERE status = 'shipped' ORDER BY coalesce(customer_id, 2147483647) DESC, tenant_id, id LIMIT 12
