-- ClickBench-like: a regular expression with a class after a literal on the url
-- label: ClickBench-like: `count(*) WHERE url ~ 'google\.com/1[0-9]'`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE url ~ 'google\.com/1[0-9]'
