-- trap: percentile_disc over a numeric column returns a numeric the final function picks from the sorted values (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, percentile_disc(0.5) WITHIN GROUP (ORDER BY n2) AS med FROM t_num GROUP BY grp ORDER BY grp
