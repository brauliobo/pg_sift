-- a usage ledger summed to a coarser grain: counters per hour, context, owner and model grouped by a truncated hour (day, month) and by its text label
-- label: `sum(cost), sum(calls)` over a 200,000-row hourly ledger `GROUP BY set_id, to_char(date_trunc('day', hour), 'YYYY-MM-DD HH24')` and `GROUP BY date_trunc('month', hour), context, set_id, model_id`, a 90-day range
-- ordered: yes
--@ setup
SET max_parallel_maintenance_workers = 0;
CREATE TABLE lr_hours (hour timestamp NOT NULL, context text NOT NULL, set_id int NOT NULL, model_id int NOT NULL, calls int NOT NULL, tokens_in bigint NOT NULL, tokens_out bigint NOT NULL, cost bigint NOT NULL,
  PRIMARY KEY (hour, context, set_id, model_id));
INSERT INTO lr_hours SELECT h, c, o, m, 1 + (hashint4(extract(epoch FROM h)::int + o * 7 + m) & 16777215) % 40, (hashint4(extract(epoch FROM h)::int + o * 11 + m) & 16777215) % 4000,
  (hashint4(extract(epoch FROM h)::int + o * 13 + m) & 16777215) % 800, (hashint4(extract(epoch FROM h)::int + o * 17 + m) & 16777215) % 90000
FROM generate_series(timestamp '2026-08-01', timestamp '2026-10-29', interval '1 hour') h, unnest(ARRAY['chat', 'report', 'embed', 'translate']) c, generate_series(1, 10) o, generate_series(1, 3) m
WHERE (hashint4(extract(epoch FROM h)::int + o * 19 + m) & 16777215) % 5 <> 0;
CREATE INDEX lr_hours_col ON lr_hours USING sift_col (hour, context, set_id, model_id, calls, tokens_in, tokens_out, cost);
VACUUM (FREEZE, ANALYZE) lr_hours
--@ pg
SELECT set_id, bucket, cost, calls FROM (
  SELECT set_id, to_char(date_trunc('day', hour), 'YYYY-MM-DD HH24') AS bucket, sum(cost) AS cost, sum(calls) AS calls FROM lr_hours
  WHERE hour >= timestamp '2026-08-01' AND hour < timestamp '2026-10-29' GROUP BY set_id, to_char(date_trunc('day', hour), 'YYYY-MM-DD HH24')
  UNION ALL
  SELECT set_id, to_char(date_trunc('month', hour), 'YYYY-MM') || ' ' || context || ' ' || model_id, sum(cost), sum(calls) FROM lr_hours
  WHERE hour >= timestamp '2026-08-01' AND hour < timestamp '2026-10-29' GROUP BY date_trunc('month', hour), context, set_id, model_id) u
ORDER BY set_id, bucket
