-- NaN stored in a numeric column as a group key and in comparisons: with a number, with NaN, between two expressions
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT n2, count(*) AS c, count(*) FILTER (WHERE n6 > n2) AS gt, count(*) FILTER (WHERE n2 = 'NaN'::numeric) AS eq, count(*) FILTER (WHERE n6 <= 'NaN'::numeric) AS le,
       count(*) FILTER (WHERE n2 + 1 <> n6) AS ne, count(*) FILTER (WHERE n2 >= 1.5) AS ge
FROM t_nan GROUP BY n2 ORDER BY n2 NULLS FIRST
