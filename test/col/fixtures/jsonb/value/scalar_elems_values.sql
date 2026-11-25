-- trap: the member of the elements of a list, declared float8 and cast from its text by the aggregate of jsonb_array_elements: numeric strings, a JSON null, NaN, Infinity and -0 as PostgreSQL reads them (the scan reads those rows again from the heap)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, count((p ->> 'v')::float8) AS nv, sum((p ->> 'v')::float8) AS s, max((p ->> 'v')::float8) AS hi
FROM t_vd, jsonb_array_elements(doc -> 'l') p WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11)
GROUP BY grp ORDER BY grp
