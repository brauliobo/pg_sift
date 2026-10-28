-- ClickBench-like: a count with a filter on a mostly-zero smallint (ClickBench query 2)
-- label: ClickBench-like: `count(*) WHERE adv_engine_id <> 0`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE adv_engine_id <> 0
