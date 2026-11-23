-- ORDER BY now() minus a timestamp: a stable function the scan evaluates once for each row, as the plan's sort does
-- shape: topn
-- expect: agg
-- key: -
SELECT id, ts FROM t_coal WHERE ts < 'infinity' ORDER BY now() - ts DESC NULLS LAST, id LIMIT 12
