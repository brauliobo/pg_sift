-- ORDER BY a varchar(3) column: many equal values, so the second key and the TID decide
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, vc FROM t_text ORDER BY vc, t DESC, id LIMIT 20
