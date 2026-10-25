-- ClickBench-like: the distinct users
-- label: ClickBench-like Q4: `count(DISTINCT user_id)` of a skewed bigint, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(DISTINCT user_id) FROM hits
