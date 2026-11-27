-- a sub-select the scan cannot tell gives one row at most is not evaluated by the scan: refused
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY CASE WHEN grp > (SELECT x FROM generate_series(1, 1) x) THEN 1 ELSE 0 END, id LIMIT 12
