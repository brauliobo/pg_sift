-- trap: ONLY the partitioned table has no rows of its own: the normal plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) AS n, sum(qty) AS q FROM ONLY t_part
