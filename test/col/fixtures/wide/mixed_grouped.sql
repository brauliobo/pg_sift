-- conjuncts on three wide leaves and a narrow one, aggregated per group
-- shape: quals
-- expect: agg
-- key: 1
SELECT grp, count(*) AS n, sum(qty) AS q, min(id) AS lo, max(id) AS hi FROM t_wide WHERE n38 > 100 AND m < '500.00' AND iv >= interval '1 day' AND qty > 3 GROUP BY grp ORDER BY grp
