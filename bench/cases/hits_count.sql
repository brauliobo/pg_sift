-- ClickBench-like: count(*) of the table
-- label: ClickBench-like Q0: `SELECT count(*) FROM hits`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits
