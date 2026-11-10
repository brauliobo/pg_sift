-- query: latest-reading-per-device-by-model
-- kind: latest_per_group
-- join: inner
-- oracle: SELECT d.model, count(*) AS devices, max(x.ts) AS last_ts, sum(x.value) AS value FROM device d JOIN (SELECT DISTINCT ON (r.device_id) r.device_id, r.ts, r.value FROM reading r ORDER BY r.device_id, r.ts DESC, r.id DESC) x ON x.device_id = d.id GROUP BY d.model
SELECT d.model, count(*) AS devices, max(x.ts) AS last_ts, sum(x.value) AS value
FROM device d JOIN LATERAL (SELECT r.ts, r.value FROM reading r WHERE r.device_id = d.id ORDER BY r.ts DESC, r.id DESC LIMIT 1) x ON true GROUP BY d.model;

-- query: latest-reading-distinct-on
-- kind: latest_per_group
-- join: inner
-- ordered: yes
SELECT x.device_id, x.ts, x.value FROM (SELECT DISTINCT ON (r.device_id) r.device_id, r.ts, r.value FROM reading r ORDER BY r.device_id, r.ts DESC, r.id DESC) x
JOIN device d ON d.id = x.device_id WHERE d.site_id <= 3 ORDER BY x.device_id;

-- query: top-devices-by-average
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT d.id, d.model, round(avg(r.value), 3) AS avg_value, count(*) AS n FROM reading r JOIN device d ON d.id = r.device_id
GROUP BY d.id, d.model ORDER BY avg_value DESC, d.id LIMIT 10;

-- query: readings-by-region
-- kind: chain
-- join: inner
SELECT s.region, count(*) AS n, sum(r.value) AS value FROM reading r JOIN device d ON d.id = r.device_id JOIN site s ON s.id = d.site_id GROUP BY s.region;

-- query: daily-readings-by-region-window
-- kind: filter_dim_attr
-- join: inner
SELECT date_trunc('day', r.ts) AS day, s.region, count(*) AS n FROM reading r JOIN device d ON d.id = r.device_id JOIN site s ON s.id = d.site_id
WHERE r.ts >= timestamp '2024-01-05' AND r.ts < timestamp '2024-01-10' GROUP BY 1, 2;

-- query: active-devices-per-model-last-days
-- kind: count_distinct_across_join
-- join: inner
SELECT d.model, count(DISTINCT r.device_id) AS devices FROM reading r JOIN device d ON d.id = r.device_id
WHERE r.ts >= (SELECT max(ts) - interval '2 days' FROM reading) GROUP BY d.model;

-- query: devices-without-recent-readings
-- kind: anti_join_count
-- join: anti
SELECT count(*) AS quiet FROM device d WHERE NOT EXISTS (SELECT 1 FROM reading r WHERE r.device_id = d.id AND r.ts >= (SELECT max(ts) - interval '1 day' FROM reading));

-- query: error-readings-by-model
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT d.model, count(*) FILTER (WHERE r.status <> 0) AS errors, count(*) FILTER (WHERE r.status = 2) AS code2, count(*) AS n
FROM reading r JOIN device d ON d.id = r.device_id GROUP BY d.model;

-- query: readings-by-region-and-model-snowflake
-- kind: snowflake
-- join: inner
SELECT s.region, d.model, count(*) AS n FROM reading r JOIN device d ON d.id = r.device_id JOIN site s ON s.id = d.site_id WHERE s.tz = 'tz1' GROUP BY s.region, d.model;

-- query: readings-before-installation
-- kind: range_join_versioned
-- join: inner
SELECT d.model, count(*) AS early FROM reading r JOIN device d ON d.id = r.device_id AND r.ts < d.installed_at + 40 GROUP BY d.model;
