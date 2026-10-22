-- simple CASE: the value is compared with each WHEN
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY CASE grp WHEN 1 THEN 10 WHEN 2 THEN 5 ELSE 0 END, id DESC LIMIT 12
