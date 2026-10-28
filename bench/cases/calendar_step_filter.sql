-- count the rows of the Wednesdays before six in the morning of a timestamp: the expression of the weekday and the hour is evaluated once for each day and each hour of the data
-- label: `count(*), sum(amount) WHERE extract(dow FROM created_at) = 3 AND extract(hour FROM created_at) < 6` over a `timestamp`
-- ordered: yes
--@ pg
SELECT count(*) AS n, sum(amount) AS total FROM orders WHERE extract(dow FROM created_at) = 3 AND extract(hour FROM created_at) < 6
