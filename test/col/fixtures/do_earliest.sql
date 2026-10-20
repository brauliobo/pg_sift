-- DISTINCT ON: the earliest order of each source, with a text column and the NULLs of total sorted first
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (source) source, id, status, total, country_code FROM orders ORDER BY source, total NULLS FIRST, placed_at, id
