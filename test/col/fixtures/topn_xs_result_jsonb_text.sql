-- ORDER BY the jsonb of a text column
-- shape: topn
-- expect: agg
-- key: -
SELECT id, t FROM t_text ORDER BY to_jsonb(t), id LIMIT 12
