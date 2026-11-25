-- right of a btrim
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY right(btrim(t), 2), id LIMIT 12
