-- uuid equality, IN, ordering (the byte order) and count distinct
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE u = md5('3')::uuid) AS eq, count(*) FILTER (WHERE u IN (md5('4')::uuid, '00000000-0000-0000-0000-000000000000')) AS in_, count(*) FILTER (WHERE u > md5('25')::uuid) AS gt, count(DISTINCT u) AS d FROM t_misc
