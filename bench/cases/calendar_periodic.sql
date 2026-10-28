-- count the rows of one month of the year, and one day of the year, of a timestamp: the units of the months are found from the span of the column and the rows they keep are not tested again
-- label: `count(*), sum(amount) WHERE extract(month FROM created_at) = 3` over a `timestamp`
-- ordered: yes
--@ pg
SELECT count(*) AS n, sum(amount) AS total FROM orders WHERE extract(month FROM created_at) = 3
