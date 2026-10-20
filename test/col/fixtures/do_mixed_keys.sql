-- DISTINCT ON: sort keys of mixed directions and NULLS placement, over columns that hold NULLs
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (source) source, id, deleted_at, total, discount, status
FROM orders ORDER BY source, deleted_at DESC NULLS LAST, total NULLS FIRST, discount DESC NULLS FIRST, id DESC
