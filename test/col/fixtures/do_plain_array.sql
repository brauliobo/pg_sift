-- trap: DISTINCT ON that selects an array column keeps the plan (the arrays are not values the aggregate carries)
-- shape: trap
-- expect: plain
-- key: -
SELECT DISTINCT ON (grp) grp, id, ia FROM t_misc ORDER BY grp, id
