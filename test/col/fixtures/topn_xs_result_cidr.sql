-- ORDER BY a cidr result: the cidr of an inet column's network
-- shape: topn
-- expect: agg
-- key: -
SELECT id, ip FROM t_net ORDER BY cidr(network(ip)) DESC, id LIMIT 12 OFFSET 1
