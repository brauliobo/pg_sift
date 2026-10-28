-- ClickBench-like: whole rows of a substring match, the first 10 by time
-- label: ClickBench-like Q24: `SELECT * WHERE url LIKE '%google%' ORDER BY event_time LIMIT 10`
-- ordered: yes
--@ pg
SELECT * FROM hits WHERE url LIKE '%google%' ORDER BY event_time, watch_id LIMIT 10
