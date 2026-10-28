-- ClickBench-like: count(*) of the table (ClickBench query 1)
-- label: ClickBench-like: `SELECT count(*) FROM hits`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits
