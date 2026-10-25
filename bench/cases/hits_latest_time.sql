-- ClickBench-like: the latest rows by time (a descending top-N of a column that follows the table)
-- label: `SELECT watch_id, event_time ORDER BY event_time DESC LIMIT 10` with no btree on it, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT watch_id, event_time FROM hits ORDER BY event_time DESC LIMIT 10
