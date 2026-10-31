-- count(x) of a column declared NOT NULL is count(*) for the scan, whether or not the index holds the column: `memo` is not in the index
-- label: `count(memo)` of a NOT NULL column outside the index, by `parent_id` (about 700 groups) of the events
-- ordered: yes
--@ pg
SELECT parent_id, count(memo) AS n FROM events GROUP BY 1 ORDER BY 1
