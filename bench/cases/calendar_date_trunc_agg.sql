-- a month report of a range of timestamps and a jsonb path: the calendar cells of the timestamp beside the leaf of the path, against an expression b-tree on the path and the timestamp
-- label: `GROUP BY date_trunc('month', ts)::date` of `ts >= '2024-03-15' AND ts < '2025-09-01' AND doc ->> 'channel' = 'ch_1'` over {rows} rows; the variant `expr_btree` is a copy of the rows with a b-tree on ((doc ->> 'channel'), ts)
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS cal_events;
CREATE TABLE cal_events (id int NOT NULL, ts timestamp NOT NULL, amount float8 NOT NULL, doc jsonb NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO cal_events SELECT i, timestamp '2024-01-01' + ((i::bigint * 7919) % 1051200) * interval '1 minute', 1 + (i % 17) + 0.25 * (i % 4),
	jsonb_build_object('channel', 'ch_' || i % 5, 'score', 1 + (i * 3) % 10) FROM generate_series(1, {rows}) i;
CREATE INDEX cal_events_col ON cal_events USING sift_col (id, ts, amount, doc) WITH (paths = 'doc.channel text');
VACUUM (FREEZE, ANALYZE) cal_events;
DROP SCHEMA IF EXISTS c_calendar_date_trunc_agg__expr_btree CASCADE;
CREATE SCHEMA c_calendar_date_trunc_agg__expr_btree;
CREATE TABLE c_calendar_date_trunc_agg__expr_btree.cal_events (LIKE cal_events) WITH (autovacuum_enabled = false);
INSERT INTO c_calendar_date_trunc_agg__expr_btree.cal_events SELECT * FROM cal_events;
CREATE INDEX cal_events_channel_ts ON c_calendar_date_trunc_agg__expr_btree.cal_events ((doc ->> 'channel'), ts);
VACUUM (FREEZE, ANALYZE) c_calendar_date_trunc_agg__expr_btree.cal_events
--@ pg
SELECT date_trunc('month', ts)::date, count(*), sum(amount) FROM cal_events WHERE ts >= date '2024-03-15' AND ts < date '2025-09-01' AND (doc ->> 'channel') = 'ch_1' GROUP BY 1 ORDER BY 1
