-- an int4 member against a bigint column, a bigint member against an int4 column and against a bigint column holding values beyond int4
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT e.grp, count(*) AS n,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.r8)) AS q_eq_r8,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q < e.r8)) AS q_lt_r8,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q8 bigint) WHERE i.q8 = e.r4)) AS q8_eq_r4,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q8 bigint) WHERE i.q8 >= e.r4)) AS q8_ge_r4,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q8 bigint) WHERE i.q8 = e.r8)) AS q8_eq_r8,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q8 bigint) WHERE i.q8 <> e.r8)) AS q8_ne_r8,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q8 bigint) WHERE i.q8 > e.r8)) AS q8_gt_r8
FROM t_eo e
GROUP BY 1
ORDER BY 1
