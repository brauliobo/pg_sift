-- ClickBench-like: a substring match on the url
-- label: ClickBench-like Q20: `count(*) WHERE url LIKE '%google%'`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE url LIKE '%google%'
