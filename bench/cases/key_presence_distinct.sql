-- count(DISTINCT key) of a whole table from the dictionaries and pre-aggregates of its granules
-- label: `count(DISTINCT segment_id)` (6,313 values, `key_order = 'segment_id'`)
--@ pg
SELECT count(DISTINCT segment_id) FROM orders
