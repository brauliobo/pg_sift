-- a volatile LIMIT keeps the normal plan
-- shape: topn_param
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders ORDER BY total, id LIMIT (random() * 0 + 10)::int
