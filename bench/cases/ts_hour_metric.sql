-- time series: one metric by hour over a week
-- label: `count(*), avg(value)` of one `metric` over 7 days, `GROUP BY date_trunc('hour', ts)`, 1,000,000 readings
-- ordered: yes
--@ pg
SELECT date_trunc('hour', ts) AS h, count(*), avg(value) FROM metrics WHERE metric = 3 AND ts >= timestamp '2025-02-10' AND ts < timestamp '2025-02-17' GROUP BY 1 ORDER BY 1
--@ timescaledb_cagg
SELECT h, sum(c), sum(s) / sum(c) FROM metrics_hourly WHERE metric = 3 AND h >= timestamp '2025-02-10' AND h < timestamp '2025-02-17' GROUP BY 1 ORDER BY 1
