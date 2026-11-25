-- an image, a lane pair and money together over the whole table
-- shape: quals
-- expect: agg
-- key: -
SELECT count(*) AS n, sum(qty) AS q, avg(qty) AS a FROM t_wide WHERE nimg > 0 AND nimg < 'Infinity' AND n19 IS NOT NULL AND m IS NOT NULL
