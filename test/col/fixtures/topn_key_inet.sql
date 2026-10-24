-- ORDER BY an inet column: IPv4 and IPv6, masks, the same address with two masks, NULLs
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ip FROM t_net ORDER BY ip, id LIMIT 12 OFFSET 1
