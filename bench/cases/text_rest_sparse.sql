-- the conditions after a `LIKE '%x%'` are tested on the rows it left, not on every string of the column: 1,000,000 pages with one url each, 0.2% of them with a word in the title
-- label: `WHERE title LIKE '%Google%' AND url NOT LIKE '%.google.%'`, grouped by country, `min(url)`, `count(DISTINCT customer)`, on 1,000,000 pages of a distinct url each
--@ setup
CREATE TABLE pages AS SELECT id, customer_id AS uid, country AS phrase,
       CASE WHEN hashint8extended(id, 1) % 500 = 0 THEN 'the Google page ' ELSE 'page ' END || id AS title,
       'http://' || CASE WHEN hashint8extended(id, 2) % 5 = 0 THEN 'maps.google.com' ELSE 'site' || (hashint8extended(id, 3) & 1023) || '.example.com' END || '/p' || id AS url
FROM orders;
CREATE INDEX pages_col ON pages USING sift_col (id, uid, phrase, title, url);
VACUUM (FREEZE, ANALYZE) pages;
--@ pg
SELECT phrase, min(url), min(title), count(*) AS c, count(DISTINCT uid) AS u FROM pages WHERE title LIKE '%Google%' AND url NOT LIKE '%.google.%' GROUP BY phrase ORDER BY c DESC, phrase LIMIT 10
