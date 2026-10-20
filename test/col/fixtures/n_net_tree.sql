-- a CASE, OR and FILTER tree over the network types together
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT CASE WHEN ip <<= '10.0.0.0/8' THEN 'ten' WHEN family(ip) = 6 THEN 'six' ELSE 'other' END AS k, count(*) AS n, count(*) FILTER (WHERE mac IS NULL OR tz > '12:00+00' OR b = B'11111111') AS f, count(DISTINCT cd) AS dc FROM t_net WHERE vb IS NOT NULL GROUP BY 1 ORDER BY 1
