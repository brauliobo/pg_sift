-- DISTINCT ON a text key with NULLs (one group), a text sort key by the collation of the column
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (country_code) country_code, id, status, lang FROM orders ORDER BY country_code NULLS FIRST, status DESC NULLS LAST, lang, id
