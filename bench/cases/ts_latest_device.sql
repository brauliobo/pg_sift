-- time series: the last 100 readings of one device (a btree on (device, ts) answers it)
-- label: `WHERE device = 42 ORDER BY ts DESC LIMIT 100`, 1,000,000 readings
-- ordered: yes
--@ pg
SELECT ts, metric, value FROM metrics WHERE device = 42 ORDER BY ts DESC, metric, value LIMIT 100
