-- a float8 member against a float8 column: NaN equals itself and is greater than every number, -0 equals 0, NULL is unknown
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(f float8) WHERE i.f = e.rf)) AS eq,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(f float8) WHERE i.f <> e.rf)) AS ne,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(f float8) WHERE i.f < e.rf)) AS lt,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(f float8) WHERE i.f >= e.rf)) AS ge,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(f float8) WHERE i.f > e.rf)) AS gt
FROM t_eo e
GROUP BY 1
ORDER BY 1
