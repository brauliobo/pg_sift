-- coalesce of two columns (no constant)
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY coalesce(i4, grp), id LIMIT 12
