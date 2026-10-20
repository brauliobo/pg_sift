-- several elements per row and one test per element: the same element must pass the comparison with the column and the one with a literal
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int, f float8) WHERE i.q = e.r4 AND i.f >= 1.5)) AS same_element,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int, f float8) WHERE i.q = e.r4)
                          AND EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int, f float8) WHERE i.f >= 1.5)) AS any_elements,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int, f float8) WHERE i.q > e.r4 AND i.q < 5 AND i.f <> 2)) AS range_of_one,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int, f float8) WHERE NOT (i.q = e.r4 AND i.f >= 1.5))) AS not_and
FROM t_eo e
GROUP BY 1
ORDER BY 1
