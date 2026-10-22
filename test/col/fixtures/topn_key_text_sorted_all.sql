-- ORDER BY a text key with a LIMIT past the heap: the sort returns every row
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t COLLATE "C", id LIMIT 100001
