-- extract of a date: a numeric result
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal WHERE d BETWEEN DATE '2000-01-01' AND DATE '2100-01-01' ORDER BY extract(month FROM d), id LIMIT 12
