-- count(DISTINCT key) of a whole table from the key presence of its units
-- label: `count(DISTINCT segment_id)` (6,313 values, `key_presence`)
--@ pg
SELECT count(DISTINCT segment_id) FROM orders
