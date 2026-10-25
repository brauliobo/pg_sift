-- ClickBench-like: the distinct search phrases (text)
-- label: ClickBench-like Q5: `count(DISTINCT search_phrase)` of a text column, 90% empty, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(DISTINCT search_phrase) FROM hits
