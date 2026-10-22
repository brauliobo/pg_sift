-- a volatile function in a CASE condition is refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY CASE WHEN random() > 2 THEN 1 ELSE grp END, id LIMIT 12
