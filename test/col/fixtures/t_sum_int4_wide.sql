-- trap: sum(int4) is a bigint past 2^31, avg(int4) a numeric
-- shape: trap
-- expect: any
-- key: -
-- scenario: yes
SELECT grp, sum(i4) AS s, avg(i4) AS a, min(i4) AS lo, max(i4) AS hi, count(i4) AS c FROM t_int GROUP BY grp ORDER BY grp
