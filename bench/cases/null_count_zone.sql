-- a count of the rows where a leaf with many values is NULL, or is not: the NULL counts of the zones of the units, not their rows
-- label: `count(*) WHERE deleted_at IS NULL` (a timestamp with many values in a unit)
--@ pg
SELECT count(*) FROM events WHERE deleted_at IS NULL
