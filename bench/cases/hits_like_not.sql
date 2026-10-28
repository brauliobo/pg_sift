-- ClickBench-like: a substring match, a negated one, a second filter and a group by
-- label: ClickBench-like Q23: `search_phrase, min(url), count(*) WHERE referer LIKE '%google%' AND url NOT LIKE '%.google.%' AND search_phrase <> '' GROUP BY 1 ORDER BY 3 DESC LIMIT 10`
-- ordered: yes
--@ pg
SELECT search_phrase, min(url), count(*) AS c FROM hits WHERE referer LIKE '%google%' AND url NOT LIKE '%.google.%' AND search_phrase <> '' GROUP BY search_phrase ORDER BY c DESC, search_phrase LIMIT 10
