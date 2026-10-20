-- FILTER of wide conditions on several aggregates
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT grp, count(*) FILTER (WHERE n38 > 100 OR nimg = 1) AS a, sum(qty) FILTER (WHERE NOT iv > interval '1 month') AS b, max(id) FILTER (WHERE n19 IS DISTINCT FROM 0) AS c FROM t_wide GROUP BY grp ORDER BY grp
