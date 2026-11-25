-- ORDER BY lower(text column): equal after the fold, ties fall to the next key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY lower(t), id LIMIT 12
