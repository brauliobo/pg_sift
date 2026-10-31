-- a scan of one table that groups is priced as starting at 55,000 operator steps: a key of a few percent of the rows grouped by month takes the scan, not the index of the key
-- label: `date_trunc('month', ts)` counted for the rows of one `skew_id` (about 3% of the table, 6,700 rows of 200,000) of the events
-- ordered: yes
--@ pg
SELECT date_trunc('month', ts) AS month, count(*) AS n FROM events WHERE skew_id = 4 GROUP BY 1 ORDER BY 1
