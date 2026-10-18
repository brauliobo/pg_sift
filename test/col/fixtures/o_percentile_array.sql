-- the array form, an interval and a text argument, a fraction that is not a constant and one out of range keep the normal plan (the last raises in both)
-- shape: trap
-- expect: plain
-- key: -
-- error: yes
SELECT grp, percentile_cont(ARRAY[0.1, 0.9]) WITHIN GROUP (ORDER BY f8) AS arr, percentile_disc(0.5) WITHIN GROUP (ORDER BY id::text) AS txt,
       percentile_cont(grp / 10.0) WITHIN GROUP (ORDER BY f8) AS dyn, percentile_cont(1.5) WITHIN GROUP (ORDER BY f8) AS bad
FROM t_float GROUP BY grp ORDER BY grp
