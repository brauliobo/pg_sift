-- inet as the WHERE of a grouped query, with an OR
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT grp, count(*) AS n, count(DISTINCT ip) AS d FROM t_net WHERE ip <<= '10.0.0.0/8' OR ip = '::1' OR ip IS NULL GROUP BY grp ORDER BY grp
