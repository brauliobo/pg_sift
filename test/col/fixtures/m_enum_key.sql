-- a key of an enum, ordered by the catalog (a value added BEFORE another)
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT e, count(*) AS n, min(qty) AS q FROM t_misc GROUP BY e ORDER BY e
