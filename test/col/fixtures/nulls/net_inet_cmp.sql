-- inet conjuncts and filters: the six comparisons, containment, overlap, IN and IS NULL
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE ip = '10.0.0.1') AS eq, count(*) FILTER (WHERE ip <> '10.0.0.1/24') AS ne, count(*) FILTER (WHERE ip < '10.0.0.2/24') AS lt, count(*) FILTER (WHERE ip <= '10.0.0.1/24') AS le, count(*) FILTER (WHERE ip > '10.0.0.1/24') AS gt, count(*) FILTER (WHERE ip >= '::1') AS ge, count(*) FILTER (WHERE ip <<= '10.0.0.0/8') AS inside, count(*) FILTER (WHERE ip >>= '10.0.0.1/32') AS contains, count(*) FILTER (WHERE ip && '10.0.0.0/24') AS overlaps, count(*) FILTER (WHERE ip IN ('::1', '2001:db8::1', '10.0.0.0/8')) AS in_, count(*) FILTER (WHERE ip IS NULL) AS nulls FROM t_net
