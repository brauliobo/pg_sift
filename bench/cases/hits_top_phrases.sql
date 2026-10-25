-- ClickBench-like: the top 10 search phrases
-- label: ClickBench-like Q12: `search_phrase, count(*) WHERE search_phrase <> '' GROUP BY 1 ORDER BY 2 DESC LIMIT 10`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT search_phrase, count(*) AS c FROM hits WHERE search_phrase <> '' GROUP BY search_phrase ORDER BY c DESC, search_phrase LIMIT 10
