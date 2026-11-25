-- an expression first, a text column and the id behind it
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, s2 FROM t_coal WHERE s2 BETWEEN -100 AND 100 ORDER BY s2 + 0, grp DESC, id LIMIT 12
