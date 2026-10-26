-- the rows of three keys of a foreign key column (6,313 values): one record of each segment's key table, not the units of the table
-- label: `count(*) WHERE segment_id IN (1234, 2345, 3456)` (6,313 values, `key_columns`)
--@ pg
SELECT count(*) FROM orders WHERE segment_id IN (1234, 2345, 3456)
