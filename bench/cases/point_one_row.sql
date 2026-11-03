-- one row by its primary key, about 20 microseconds: the statement the instruction count must be able to measure (it is run again and again in one backend between two readings of perf and divided)
-- label: `SELECT amount FROM orders WHERE id = 4242`
--@ pg
SELECT amount FROM orders WHERE id = 4242
