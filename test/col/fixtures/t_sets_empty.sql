-- trap: the empty grouping set over no row is one row (as a plain aggregate), the other sets give none; GROUPING() of it is all ones
-- shape: trap
-- expect: agg
-- key: -
SELECT o.lang, grouping(o.lang) AS g, count(*) AS n, sum(o.store_id) AS s, min(o.placed_at) AS d, count(DISTINCT o.source) AS src
FROM orders o WHERE o.lang = 'zz'
GROUP BY GROUPING SETS ((o.lang), ()) ORDER BY 1, 2
