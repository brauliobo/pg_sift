-- timestamp plus an interval
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts FROM t_coal ORDER BY ts + interval '1 day', id LIMIT 12
