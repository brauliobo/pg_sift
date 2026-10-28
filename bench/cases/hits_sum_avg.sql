-- ClickBench-like: a sum, a count and an average (ClickBench query 3)
-- label: ClickBench-like: `sum(adv_engine_id), count(*), avg(resolution_width)`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT sum(adv_engine_id), count(*), avg(resolution_width) FROM hits
