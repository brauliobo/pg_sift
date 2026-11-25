-- SELECT DISTINCT of a predicate on a nullable column and a nullable key
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT deleted_at IS NULL AS live, discount IS NULL AS nodiscount, lang FROM orders ORDER BY 1, 2, 3 NULLS FIRST
