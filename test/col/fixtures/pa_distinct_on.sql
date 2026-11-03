-- DISTINCT ON of a partitioned table with no unique index: the ORDER BY ends in the id, so the row is determined
-- shape: part
-- expect: agg
-- key: -
SELECT DISTINCT ON (grp) grp, d, id FROM t_part ORDER BY grp, d DESC, id
