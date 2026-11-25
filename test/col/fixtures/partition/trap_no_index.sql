-- trap: a partition without a column index: the normal plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_part_ni GROUP BY grp ORDER BY grp
