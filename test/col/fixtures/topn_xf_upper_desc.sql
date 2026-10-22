-- ORDER BY upper(text column) DESC NULLS FIRST (an accent, a sharp s, a multi-byte letter)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY upper(t) DESC NULLS FIRST, id LIMIT 12
