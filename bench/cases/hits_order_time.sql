-- ClickBench-like: the first 10 rows by time (ClickBench query 25)
-- label: ClickBench-like: `SELECT watch_id, event_time ORDER BY event_time LIMIT 10` with no btree on it, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT watch_id, event_time FROM hits ORDER BY event_time, watch_id LIMIT 10
