-- a queue's depth over time from three kinds of timestamps of the same rows: one `UNION ALL` arm per timestamp, bucketed with date_bin, summed per bucket and kind
-- label: `date_bin('15 minutes', event_at, origin)` over three `UNION ALL` arms (created, started, completed) of a 100,000-row job table, kind IN (..), a 4-day range, a membership test per arm
-- ordered: yes
--@ setup
CREATE TABLE tb_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO tb_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX tb_links_set ON tb_links (set_id, parent_id);
CREATE TABLE tb_jobs AS SELECT i AS id, 1 + i % (SELECT count(*) FROM dims)::int AS parent_id, (ARRAY['scheduled', 'scheduled', 'scheduled', 'manual', 'retry', 'backfill'])[1 + i % 6] AS kind,
  t AS created_at, CASE WHEN i % 7 <> 0 THEN t + (i % 30) * interval '1 minute' END AS started_at, CASE WHEN i % 7 NOT IN (0, 1) THEN t + (30 + i % 40) * interval '1 minute' END AS completed_at
  FROM (SELECT i, timestamp '2025-12-01' + (hashint4(i) & 16777215) % 2592000 * interval '1 second' AS t FROM generate_series(1, 100000) i) q;
ALTER TABLE tb_jobs ADD PRIMARY KEY (id);
CREATE INDEX tb_jobs_created ON tb_jobs (created_at);
CREATE INDEX tb_jobs_col ON tb_jobs USING sift_col (parent_id, kind, created_at, started_at, completed_at);
VACUUM (FREEZE, ANALYZE) tb_jobs;
ANALYZE tb_links
--@ pg
SELECT date_bin('15 minutes'::interval, ev.event_at, timestamp '2000-01-01') AS bucket, ev.kind, ev.event_order, sum(ev.queued_delta) AS queued_delta, sum(ev.active_delta) AS active_delta
FROM (
  SELECT j.created_at AS event_at, j.kind, 1 AS queued_delta, 0 AS active_delta, 0 AS event_order FROM tb_jobs j
    WHERE j.kind IN ('scheduled', 'manual') AND j.created_at >= timestamp '2025-12-25' AND j.created_at < timestamp '2025-12-29' AND EXISTS (SELECT 1 FROM tb_links l WHERE l.set_id = 4 AND l.parent_id = j.parent_id)
  UNION ALL
  SELECT j.started_at, j.kind, -1, 1, 1 FROM tb_jobs j
    WHERE j.kind IN ('scheduled', 'manual') AND j.started_at >= timestamp '2025-12-25' AND j.started_at < timestamp '2025-12-29' AND EXISTS (SELECT 1 FROM tb_links l WHERE l.set_id = 4 AND l.parent_id = j.parent_id)
  UNION ALL
  SELECT coalesce(j.completed_at, j.started_at + interval '1 hour'), j.kind, 0, -1, 2 FROM tb_jobs j
    WHERE j.kind IN ('scheduled', 'manual') AND j.started_at IS NOT NULL AND coalesce(j.completed_at, j.started_at + interval '1 hour') >= timestamp '2025-12-25' AND coalesce(j.completed_at, j.started_at + interval '1 hour') < timestamp '2025-12-29'
      AND EXISTS (SELECT 1 FROM tb_links l WHERE l.set_id = 4 AND l.parent_id = j.parent_id)
) ev GROUP BY 1, 2, 3 ORDER BY 1, 2, 3
