-- coalesce of a timestamp and a date column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts, d FROM t_coal ORDER BY coalesce(ts, d), id LIMIT 12
