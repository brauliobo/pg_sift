-- lower() of a varchar column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, vc FROM t_text ORDER BY lower(vc), id DESC LIMIT 12
