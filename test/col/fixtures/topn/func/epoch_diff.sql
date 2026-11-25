-- extract of the difference of two timestamps
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts FROM t_coal WHERE ts < 'infinity' ORDER BY extract(epoch FROM ts - TIMESTAMP '2024-02-01') DESC, id LIMIT 12
