-- the first rows by time and a column the index does not hold, as a top-N of the first key under an incremental sort
-- label: `SELECT watch_id, event_time ORDER BY event_time DESC, watch_id DESC LIMIT 200`, `watch_id` not in the index and no btree on `event_time`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT watch_id, event_time FROM hits ORDER BY event_time DESC, watch_id DESC LIMIT 200
