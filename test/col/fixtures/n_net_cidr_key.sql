-- a key of cidr values with a containment test against an inet column
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT cd, count(*) AS n, count(*) FILTER (WHERE cd >>= ip) AS holds, count(DISTINCT ip) AS d FROM t_net GROUP BY cd ORDER BY cd
