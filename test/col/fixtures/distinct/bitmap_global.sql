-- the same without a group: one bitmap
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(DISTINCT v) AS d, count(DISTINCT w) AS wide, count(DISTINCT x) AS x FROM t_dist
