-- trap: the indexes of two partitions hold other leaves (a path of another type): the normal plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, count(*) AS n, sum(qty) AS q, sum((doc ->> 'k')::int) AS k FROM t_part_op GROUP BY grp ORDER BY grp
