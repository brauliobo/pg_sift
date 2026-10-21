-- trap: a number where an array leaf is declared raises the error of jsonb_array_length, not a NULL
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) AS n, sum(jsonb_array_length(doc -> 'n')) AS s
FROM t_vd WHERE grp = 6
