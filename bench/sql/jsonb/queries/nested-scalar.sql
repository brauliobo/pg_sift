SELECT (d.attrs #>> '{nested,a,b}')::int AS b, count(*) FROM docs d GROUP BY 1 ORDER BY 1
