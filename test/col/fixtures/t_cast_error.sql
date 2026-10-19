-- trap: a cast that raises an error (bigint to int4) on some rows
-- shape: trap
-- expect: any
-- error: yes
-- key: -
SELECT grp, sum(i8::int4) AS s FROM t_int GROUP BY grp ORDER BY grp
