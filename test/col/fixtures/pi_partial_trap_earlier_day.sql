-- trap: a bound earlier than the index's: rows of the first week are in the heap only
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n FROM t_partial WHERE deleted_at IS NULL AND day >= '2026-01-02' GROUP BY grp ORDER BY grp
