-- count(DISTINCT key) of a whole table from the key lists and bitmaps of its units
-- label: `count(DISTINCT segment_id)` (6,313 values, `key_columns`)
--@ pg
SELECT count(DISTINCT segment_id) FROM orders
