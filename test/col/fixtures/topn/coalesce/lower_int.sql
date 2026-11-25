-- lower() of the text column then an integer key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, t FROM t_text ORDER BY grp DESC, lower(t), id LIMIT 12
