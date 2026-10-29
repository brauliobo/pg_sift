-- ClickBench-like: whole rows of a substring match, the first 10 by time (ClickBench query 24)
-- label: ClickBench-like: `SELECT * WHERE url LIKE '%google%' ORDER BY event_time LIMIT 10`
-- ordered: yes
--@ pg
SELECT * FROM hits WHERE url LIKE '%google%' ORDER BY event_time, watch_id LIMIT 10
--@ es
-- Elasticsearch SQL returns `SELECT *` with the columns in name order, so the columns are listed in the order of the table
SELECT watch_id, event_time, event_date, counter_id, client_ip, region_id, user_id, os, user_agent, url, referer, is_refresh, resolution_width, mobile_phone_model, search_phrase, adv_engine_id FROM hits WHERE url LIKE '%google%' ORDER BY event_time, watch_id LIMIT 10
