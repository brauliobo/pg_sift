-- a thirty-day window, not aligned to a month (a month is answered from the calendar pre-aggregates), of a timestamp column that no unit's zone cuts (the rows arrive in no order of it): the sorted run of each unit finds the window by binary search
-- label: `count(*), sum(amount) WHERE created_at` in a thirty-day window, `order_columns = 'created_at'`
-- ordered: yes
--@ pg
SELECT count(*) AS n, sum(amount) AS revenue FROM orders WHERE created_at >= timestamp '2024-06-10 13:00' AND created_at < timestamp '2024-07-10 13:00'
