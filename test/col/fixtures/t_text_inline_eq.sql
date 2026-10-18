-- flat: text longer than a dictionary entry: equality and inequality run on the encoded strings, the groups keep the long values
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, count(DISTINCT t) AS d, min(t COLLATE "C") AS lo, max(t COLLATE "C") AS hi, sum(id) AS s
FROM t_text WHERE t <> repeat('c', 330) || '2' GROUP BY grp ORDER BY grp
