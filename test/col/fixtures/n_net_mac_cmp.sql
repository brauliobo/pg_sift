-- macaddr and macaddr8 conjuncts, trunc and IN
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE mac = '08:00:2b:01:02:03') AS eq, count(*) FILTER (WHERE mac < 'aa:bb:cc:dd:ee:ff') AS lt, count(*) FILTER (WHERE mac >= 'aa:bb:cc:dd:ee:ff') AS ge, count(*) FILTER (WHERE mac <> '00:00:00:00:00:00') AS ne, count(*) FILTER (WHERE mac8 = '0800.2b01.0203.0405') AS eq8, count(*) FILTER (WHERE mac8 > '08:00:2b:ff:fe:01:02:03') AS gt8, count(*) FILTER (WHERE mac IN ('ff:ff:ff:ff:ff:ff', '00:00:00:00:00:00')) AS in_, count(DISTINCT trunc(mac)) AS tr, count(DISTINCT mac8) AS d8 FROM t_net
