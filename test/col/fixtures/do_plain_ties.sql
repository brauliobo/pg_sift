-- trap: DISTINCT ON whose ORDER BY has no unique column keeps the plan (rows that tie are picked in the order of PostgreSQL's sort); the select list holds the sort columns only, so the result is the same
-- shape: trap
-- expect: plain
-- key: -
SELECT DISTINCT ON (store_id) store_id, placed_at FROM orders WHERE store_id < 50 ORDER BY store_id, placed_at DESC
