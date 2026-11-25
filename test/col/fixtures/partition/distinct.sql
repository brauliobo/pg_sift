-- SELECT DISTINCT over a partitioned table
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT grp, tag FROM t_part WHERE qty < 30 ORDER BY grp, tag
