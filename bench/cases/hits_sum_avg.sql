-- ClickBench-like: a sum, a count and an average
-- label: ClickBench-like Q2: `sum(adv_engine_id), count(*), avg(resolution_width)`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT sum(adv_engine_id), count(*), avg(resolution_width) FROM hits
