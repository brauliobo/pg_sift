-- a FILTER clause on a wide leaf
-- shape: expr
-- expect: plain
-- key: -
SELECT grp, count(*) FILTER (WHERE m > '1.00') AS n FROM t_wide GROUP BY grp ORDER BY grp
