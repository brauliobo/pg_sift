-- count the rows of one day, and of the months after one, of a timestamp: the calendar step is a range of the column that the kernels compare with the zones of the units
-- label: `count(*), sum(amount) WHERE created_at::date = D` over a `timestamp`
-- ordered: yes
--@ pg
SELECT count(*) AS n, sum(amount) AS total FROM orders WHERE created_at::date = date '2024-06-15'
