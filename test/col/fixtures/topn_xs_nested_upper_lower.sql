-- the other order, descending
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY upper(lower(t)) DESC, id LIMIT 12
