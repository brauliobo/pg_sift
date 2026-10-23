-- a timestamp plus an interval made of a column (the interval is not stored)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ts, grp FROM t_coal WHERE ts < 'infinity' ORDER BY ts + grp * interval '1 hour', id LIMIT 12
