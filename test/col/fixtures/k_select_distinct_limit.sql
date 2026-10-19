-- SELECT DISTINCT with the order and the limit above it
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT store_id FROM orders ORDER BY store_id DESC LIMIT 5
