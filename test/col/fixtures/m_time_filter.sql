-- conjuncts, a BETWEEN, an OR and a FILTER on time
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, count(*) FILTER (WHERE t BETWEEN '01:00' AND '11:00') AS a, count(*) FILTER (WHERE t < '00:00:00.000003' OR t = '24:00:00') AS b, min(t) AS mn, max(t) AS mx FROM t_misc WHERE t > '00:00:00' OR grp = 1 GROUP BY grp ORDER BY grp
