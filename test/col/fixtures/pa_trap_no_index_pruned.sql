-- a partition without a column index that the query prunes: the others are read
-- shape: part
-- expect: agg
-- key: -
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_part_ni WHERE d < DATE '2024-07-01' GROUP BY grp ORDER BY grp
