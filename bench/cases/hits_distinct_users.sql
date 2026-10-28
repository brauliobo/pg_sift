-- ClickBench-like: the distinct users (ClickBench query 5)
-- label: ClickBench-like: `count(DISTINCT user_id)` of a skewed bigint, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(DISTINCT user_id) FROM hits
