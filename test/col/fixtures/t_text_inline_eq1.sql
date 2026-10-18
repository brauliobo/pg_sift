-- flat: an equality on a text longer than a dictionary entry, with a second filter on the dictionary leaf
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, sum(id) AS s FROM t_text WHERE t = repeat('c', 330) || '2' AND vc = 'lng' GROUP BY grp ORDER BY grp
