-- NULLS FIRST on an ascending numeric key
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, total, discount FROM orders WHERE memo @@> 'late OR missing' ORDER BY total NULLS FIRST, id DESC LIMIT 15
