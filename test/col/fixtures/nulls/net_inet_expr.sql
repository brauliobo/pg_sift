-- functions of inet as the key and the argument: host, family, masklen, network, text
-- shape: expr
-- expect: agg
-- key: -
SELECT host(ip) AS h, family(ip) AS f, masklen(ip) AS m, count(*) AS n, max(network(ip)::text) AS net FROM t_net GROUP BY 1, 2, 3 ORDER BY 1, 2, 3
