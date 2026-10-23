-- count(DISTINCT) of a key that is unique: the set holds one entry per row
-- label: `count(DISTINCT id) GROUP BY country` over a unique `bigint` key
-- ordered: yes
--@ pg
SELECT country, count(DISTINCT id) AS orders FROM orders GROUP BY country ORDER BY country
