-- ORDER BY a text key past the rows the heap holds: every row that passes is sorted (a tuplesort of the entries)
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t DESC, id LIMIT 100 OFFSET 100000
