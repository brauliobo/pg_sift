-- date_trunc of a timestamp
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts FROM t_coal WHERE ts < 'infinity' ORDER BY date_trunc('month', ts) DESC, id LIMIT 12
