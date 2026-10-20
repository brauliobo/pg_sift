-- keys and aggregates of several of these types together with a text-free filter
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT e, grp, count(*) AS n, count(DISTINCT u) AS du, max(t) AS mx FROM t_misc WHERE ia IS NOT NULL OR u IS NOT NULL GROUP BY e, grp ORDER BY e, grp
