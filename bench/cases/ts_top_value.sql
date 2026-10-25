-- time series: the ten highest readings (a top-N of a float column no btree orders)
-- label: `SELECT ts, device, value ORDER BY value DESC LIMIT 10`, 1,000,000 readings
-- ordered: yes
--@ pg
SELECT ts, device, value FROM metrics ORDER BY value DESC, ts, device LIMIT 10
