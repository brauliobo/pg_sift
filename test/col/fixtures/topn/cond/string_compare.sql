-- comparison of two string columns
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY t < vc, t = vc, id LIMIT 12
