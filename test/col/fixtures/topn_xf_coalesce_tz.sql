-- coalesce of a timestamptz column and a constant timestamptz, then a second key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tz FROM t_coal ORDER BY coalesce(tz, TIMESTAMPTZ '2024-01-05 00:00:00+00') DESC NULLS LAST, grp DESC, id LIMIT 12
