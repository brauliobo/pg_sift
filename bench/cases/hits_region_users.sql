-- ClickBench-like: top regions by distinct users
-- label: ClickBench-like Q8: `region_id, count(DISTINCT user_id) GROUP BY region_id ORDER BY 2 DESC LIMIT 10`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT region_id, count(DISTINCT user_id) AS u FROM hits GROUP BY region_id ORDER BY u DESC, region_id LIMIT 10
