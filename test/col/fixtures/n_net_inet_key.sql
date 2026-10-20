-- a key of inet values (mask, IPv4-mapped IPv6, the same address spelled two ways, NULL) and its text
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT ip, count(*) AS n FROM t_net GROUP BY ip ORDER BY ip
