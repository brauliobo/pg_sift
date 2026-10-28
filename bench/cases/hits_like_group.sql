-- ClickBench-like: a substring match, a second filter and a group by (ClickBench query 22)
-- label: ClickBench-like: `search_phrase, min(url), count(*) WHERE url LIKE '%google%' AND search_phrase <> '' GROUP BY 1 ORDER BY 3 DESC LIMIT 10`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT search_phrase, min(url), count(*) AS c FROM hits WHERE url LIKE '%google%' AND search_phrase <> '' GROUP BY search_phrase ORDER BY c DESC, search_phrase LIMIT 10
