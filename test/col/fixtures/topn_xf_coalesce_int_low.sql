-- the constant below every value: the NULL rows come first, tied on it
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, customer_id FROM orders ORDER BY coalesce(customer_id, -1), id LIMIT 12
