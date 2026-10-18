-- flat: text longer than a dictionary entry, grouped, counted, filtered and ordered bytewise
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, count(t) AS nt, count(DISTINCT t) AS d, min(t COLLATE "C") AS lo, max(t COLLATE "C") AS hi
FROM t_text WHERE t IS NULL OR length(t) > 200 OR t = 'ab' GROUP BY grp ORDER BY grp
