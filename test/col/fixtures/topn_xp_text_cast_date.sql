-- the text of a date depends on DateStyle (stable): refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal ORDER BY d::text COLLATE "C", id LIMIT 12
