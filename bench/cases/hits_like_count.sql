-- ClickBench-like: a substring match on the url (ClickBench query 21)
-- label: ClickBench-like: `count(*) WHERE url LIKE '%google%'`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE url LIKE '%google%'
