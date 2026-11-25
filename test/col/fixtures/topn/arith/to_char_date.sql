-- ORDER BY to_char of a date: a stable function of the session's locale
-- shape: topn
-- expect: agg
-- key: -
SELECT id, d FROM t_coal ORDER BY to_char(d, 'DD Mon YYYY'), id LIMIT 12
