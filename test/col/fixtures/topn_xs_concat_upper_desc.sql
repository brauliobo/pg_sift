-- upper of a concatenation, descending
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY upper(vc || t) DESC, id LIMIT 12
