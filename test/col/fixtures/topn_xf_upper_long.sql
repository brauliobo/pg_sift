-- upper() of strings longer than a dictionary entry that share a long prefix
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, length(t) FROM t_text WHERE grp = 1 AND t > repeat('a', 100) ORDER BY upper(t), id LIMIT 12
