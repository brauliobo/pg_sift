-- a cidr key that the same query also reads as inet (the operators of cidr are inet's): the leaf would have two types, the normal plan stays
-- shape: expr
-- expect: plain
-- key: -
SELECT cd, count(*) AS n, count(*) FILTER (WHERE cd = '10.0.0.0/8') AS eq FROM t_net GROUP BY cd ORDER BY cd
