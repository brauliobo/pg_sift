-- SELECT DISTINCT with the order and the limit above it
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT tenant_id FROM orders ORDER BY tenant_id DESC LIMIT 5
