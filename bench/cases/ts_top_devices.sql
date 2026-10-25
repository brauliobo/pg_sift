-- time series: the ten devices of the highest average over the last week
-- label: `device, avg(value)` over the last 7 days, `GROUP BY device ORDER BY 2 DESC LIMIT 10`, 1,000,000 readings
-- ordered: yes
--@ pg
SELECT device, avg(value) AS a FROM metrics WHERE ts >= timestamp '2025-03-24' GROUP BY device ORDER BY a DESC, device LIMIT 10
