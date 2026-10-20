-- LIMIT of a sub-select: not the same at every execution, the normal plan
-- shape: topn_param
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders ORDER BY total, id LIMIT (SELECT 10)
