-- ClickBench-like: a regular expression that is a literal on the url
-- label: ClickBench-like: `count(*) WHERE url ~ 'google\.com/12'`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE url ~ 'google\.com/12'
