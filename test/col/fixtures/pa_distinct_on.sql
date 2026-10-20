-- trap: DISTINCT ON of a partitioned table with no unique index keeps the plan (a tie would be PostgreSQL's choice)
-- shape: part
-- expect: plain
-- key: -
SELECT DISTINCT ON (grp) grp, d, id FROM t_part ORDER BY grp, d DESC, id
