-- lower() under a filter and a LIMIT with an OFFSET
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text WHERE grp = 2 ORDER BY lower(t), id LIMIT 6 OFFSET 4
