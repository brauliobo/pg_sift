-- coalesce of two text columns and a constant
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY coalesce(t, vc, 'zzz'), id LIMIT 12
