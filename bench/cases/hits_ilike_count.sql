-- ClickBench-like: a case-insensitive substring match on the url
-- label: ClickBench-like: `count(*) WHERE url ILIKE '%GOOGLE%'`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE url ILIKE '%GOOGLE%'
