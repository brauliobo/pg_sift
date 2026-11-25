-- macaddr and macaddr8 keys (the same address in four spellings is one value), comparisons and count distinct
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT mac, mac8, count(*) AS n, count(DISTINCT mac) AS dm FROM t_net GROUP BY mac, mac8 ORDER BY mac, mac8
