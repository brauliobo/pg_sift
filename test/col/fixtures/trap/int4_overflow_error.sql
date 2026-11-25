-- trap: integer overflow in an expression under sum(): the error of PostgreSQL (22003) in every plan
-- shape: trap
-- expect: any
-- error: yes
-- key: -
SELECT sum(i4 * 2) AS s FROM t_int WHERE grp = 1
