-- an element tested against a column of its own row: = <> < <= > >= (the column and the member are int4), NULL columns, JSON null and missing members
-- shape: items-exists-same-element
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.r4)) AS eq,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q <> e.r4)) AS ne,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q < e.r4)) AS lt,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q <= e.r4)) AS le,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q > e.r4)) AS gt,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q >= e.r4)) AS ge
FROM t_eo e
GROUP BY 1
ORDER BY 1
