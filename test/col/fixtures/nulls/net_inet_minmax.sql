-- min, max and count distinct of inet and cidr (cidr uses the inet aggregates)
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT grp, min(ip) AS lo, max(ip) AS hi, min(cd) AS clo, max(cd) AS chi, count(DISTINCT ip) AS d, count(DISTINCT cd) AS dc FROM t_net GROUP BY grp ORDER BY grp
