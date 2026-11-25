-- a per-group (column-valued) fraction keeps the normal plan, and so does the out of range constant of the same query (the last raises in both); the array form and a text argument are generic aggregates (g_ordered_set)
-- shape: trap
-- expect: plain
-- key: -
-- error: yes
SELECT grp, percentile_cont(ARRAY[0.1, 0.9]) WITHIN GROUP (ORDER BY f8) AS arr, percentile_disc(0.5) WITHIN GROUP (ORDER BY id::text) AS txt,
       percentile_cont(grp / 10.0) WITHIN GROUP (ORDER BY f8) AS dyn, percentile_cont(1.5) WITHIN GROUP (ORDER BY f8) AS bad
FROM t_float GROUP BY grp ORDER BY grp
