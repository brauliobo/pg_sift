-- DISTINCT of a bit and a varbit column together
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT * FROM (SELECT DISTINCT b, vb FROM t_net) s ORDER BY b, vb
