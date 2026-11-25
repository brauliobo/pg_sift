-- ORDER BY an integer then a text key, and a text then an integer key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, t FROM t_text ORDER BY grp DESC, t, id LIMIT 18
