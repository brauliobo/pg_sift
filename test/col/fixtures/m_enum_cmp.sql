-- comparisons, min, max and count distinct of an enum
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT grp, count(*) FILTER (WHERE e < 'ok') AS lt, count(*) FILTER (WHERE e >= 'ok' AND e <> 'happy') AS mid, min(e)::text AS mn, max(e)::text AS mx, count(DISTINCT e) AS d FROM t_misc GROUP BY grp ORDER BY grp
