-- ClickBench-like: the distinct search phrases (text) (ClickBench query 6)
-- label: ClickBench-like: `count(DISTINCT search_phrase)` of a text column, 90% empty, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(DISTINCT search_phrase) FROM hits
