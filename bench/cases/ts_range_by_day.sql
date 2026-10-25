-- time series: a fortnight of readings grouped by day
-- label: `count(*), avg(value), max(value)` over 14 days of `ts`, `GROUP BY date_trunc('day', ts)`, 1,000,000 readings
-- ordered: yes
--@ pg
SELECT date_trunc('day', ts) AS d, count(*), avg(value), max(value) FROM metrics WHERE ts >= timestamp '2025-02-01' AND ts < timestamp '2025-02-15' GROUP BY 1 ORDER BY 1
