-- a sub-select is not evaluated by the scan: refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY CASE WHEN grp > (SELECT 1) THEN 1 ELSE 0 END, id LIMIT 12
