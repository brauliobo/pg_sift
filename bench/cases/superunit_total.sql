-- a whole-table total from the summary of a group of units (superunit)
-- label: `count(*), sum(amount)` of the whole table
--@ pg
SELECT count(*), sum(amount) FROM orders
