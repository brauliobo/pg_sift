-- array operators, ANY, cardinality and distinct counts
-- shape: expr
-- expect: agg
-- scenario: yes
-- key: -
SELECT grp, count(*) FILTER (WHERE ia @> ARRAY[1]) AS contains, count(*) FILTER (WHERE ta && ARRAY['a1']) AS overlaps, count(*) FILTER (WHERE 3 = ANY (ia)) AS any_, sum(cardinality(ta)) AS card, count(DISTINCT ia) AS d, count(DISTINCT ta) AS dt FROM t_misc GROUP BY grp ORDER BY grp
