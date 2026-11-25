-- trap: the FILTER of an aggregate casts a path declared float8 from its text: numeric strings, a JSON null, NaN, Infinity and -0 pass or fail it as PostgreSQL reads them (the scan reads those rows again from the heap)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, count(*) FILTER (WHERE (doc ->> 'k')::float8 > 1) AS hi, count(*) FILTER (WHERE (doc ->> 'k')::float8 IS NULL) AS nl
FROM t_vd WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11)
GROUP BY grp ORDER BY grp
