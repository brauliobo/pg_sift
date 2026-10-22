-- coalesce of a timestamp column (infinity among the values) and a constant timestamp
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts FROM t_coal ORDER BY coalesce(ts, TIMESTAMP '2024-01-10 12:00:00'), id LIMIT 12
