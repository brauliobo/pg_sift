-- ORDER BY a numeric(19+) column plus a constant: the wide leaf read through its dictionary of datums
-- shape: topn
-- expect: agg
-- key: -
SELECT id, n19 FROM t_wide ORDER BY n19 + 1 DESC NULLS LAST, id LIMIT 12
