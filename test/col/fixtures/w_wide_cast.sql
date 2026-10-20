-- the text of a numeric image shows the display scale, which an equal number may not share; the scan keeps the spelling of each value
-- shape: expr
-- expect: agg
-- key: -
SELECT nimg::text, count(*) FROM t_wide WHERE nimg = 1 GROUP BY nimg::text ORDER BY 1
