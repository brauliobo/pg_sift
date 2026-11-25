-- IS DISTINCT FROM, ANY of an array and a cast
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE n38 IS DISTINCT FROM 0) AS a, count(*) FILTER (WHERE nimg = ANY (ARRAY[1, 1.5, 100])) AS b, count(*) FILTER (WHERE n19::float8 > 1) AS c, count(*) FILTER (WHERE m::numeric > 1) AS d FROM t_wide WHERE n19 <> 'NaN'
