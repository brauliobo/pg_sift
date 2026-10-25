CREATE MATERIALIZED VIEW metrics_hourly WITH (timescaledb.continuous) AS SELECT time_bucket('1 hour', ts) AS h, metric, count(*) AS c, sum(value) AS s, max(value) AS m FROM metrics GROUP BY 1, 2 WITH NO DATA;
CALL refresh_continuous_aggregate('metrics_hourly', NULL, NULL);
ANALYZE metrics_hourly
