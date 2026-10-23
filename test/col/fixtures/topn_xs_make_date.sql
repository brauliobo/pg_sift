-- a date made of columns
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY make_date(2024, 1 + grp * 3, 1) DESC, id LIMIT 12
