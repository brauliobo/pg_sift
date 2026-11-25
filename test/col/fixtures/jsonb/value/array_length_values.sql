-- trap: the length of an array leaf whose rows hold arrays (the index flags the values its float8 leaves cannot store, so the scan keeps the normal plan)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum(jsonb_array_length(doc -> 'n')) AS s
FROM t_vd WHERE grp NOT IN (2, 3, 5, 6)
GROUP BY grp ORDER BY grp
