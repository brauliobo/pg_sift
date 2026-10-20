-- trap: DISTINCT ON whose ORDER BY has no unique tie-break keeps the normal plan (the select list holds the sort columns only: any tied row gives the same result)
-- shape: trap
-- expect: plain
-- key: -
SELECT DISTINCT ON (store_id) store_id, placed_at FROM orders WHERE store_id < 50 ORDER BY store_id, placed_at DESC
