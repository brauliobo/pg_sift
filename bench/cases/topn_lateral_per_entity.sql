-- time series: the last 3 readings of each of 20 devices, one top-N scan for each device that an enclosing query's row sets, with no btree on ts or on (device, ts)
-- label: `CROSS JOIN LATERAL (.. WHERE device = d.device ORDER BY ts DESC LIMIT 3)` for 20 devices, 1,000,000 readings, no btree on `ts` or `(device, ts)`
-- ordered: yes
--@ setup
DROP INDEX IF EXISTS metrics_device_ts, metrics_ts
--@ pg
SELECT d.device, x.ts, x.metric, x.value FROM generate_series(1, 20) d(device)
CROSS JOIN LATERAL (SELECT ts, metric, value FROM metrics WHERE device = d.device ORDER BY ts DESC, metric, value LIMIT 3) x
ORDER BY d.device, x.ts DESC, x.metric, x.value
