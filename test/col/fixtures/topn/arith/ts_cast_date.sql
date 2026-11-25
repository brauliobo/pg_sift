-- timestamp to date
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts FROM t_coal ORDER BY ts::date DESC, id LIMIT 12
