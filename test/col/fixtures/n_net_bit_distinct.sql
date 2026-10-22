-- DISTINCT of a bit and a varbit column, alone and together (NULLs and values of every length)
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT * FROM (SELECT DISTINCT vb FROM t_net) s ORDER BY vb
