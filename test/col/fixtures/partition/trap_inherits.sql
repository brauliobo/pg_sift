-- trap: an inheritance parent (INHERITS, not partitions): the normal plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_inh GROUP BY grp ORDER BY grp
