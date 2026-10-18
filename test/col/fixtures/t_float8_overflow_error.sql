-- trap: a float8 sum that overflows (22003: value out of range) in every plan
-- shape: trap
-- expect: any
-- error: yes
-- key: -
SELECT sum(f8) AS s FROM t_float WHERE grp = 6
