-- a point column no index holds: a query that reads it keeps the normal plan, one that does not still uses the index
-- shape: expr
-- expect: plain
-- key: -
SELECT pt::text AS p, count(*) AS n, count(DISTINCT ip) AS d FROM t_net GROUP BY pt::text ORDER BY 1
