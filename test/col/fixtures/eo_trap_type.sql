-- trap: an int4 member against a float8 column is compared as float8: the member goes through a cast, which is not the leaf
-- shape: trap
-- expect: plain
-- key: -
SELECT e.grp, count(*) AS n
FROM t_eo e
WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(e.doc) AS i(q int) WHERE i.q = e.rf)
GROUP BY 1
ORDER BY 1
