-- a column, a separator and the text of the id
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t || '-' || id::text, id LIMIT 12
