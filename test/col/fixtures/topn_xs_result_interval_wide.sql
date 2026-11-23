-- ORDER BY an interval column plus an interval: an interval result, 1 mon and 30 days tie
-- shape: topn
-- expect: agg
-- key: -
SELECT id, iv FROM t_wide ORDER BY iv + interval '1 day', id LIMIT 12
