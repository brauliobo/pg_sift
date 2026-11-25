-- range and multirange operators and functions
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, count(*) FILTER (WHERE r && int4range(3, 5)) AS overlaps, count(*) FILTER (WHERE r @> 7) AS has7, count(*) FILTER (WHERE mr @> 25) AS mr25, count(*) FILTER (WHERE tr @> '2020-02-01'::timestamptz) AS tr_in, count(DISTINCT tr) AS dtr, count(DISTINCT mr) AS dmr FROM t_misc GROUP BY grp ORDER BY grp
