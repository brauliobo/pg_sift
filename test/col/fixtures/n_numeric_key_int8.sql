-- group by a bigint column, the key goes through the same scaled-integer ids
-- shape: notes-path-exists
-- expect: agg
-- key: -
SELECT i8, count(*) AS n, sum(i4) AS s
FROM t_int
GROUP BY i8
ORDER BY i8 NULLS FIRST
