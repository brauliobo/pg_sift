-- the open rows of a set of parents counted per dimension row and state with conditional sums
-- label: `sum(CASE WHEN status = 'scheduled' ..), sum(CASE WHEN status = 'started' AND started_at >= cutoff ..)` grouped by the source, over the open rows of the 100,000-row job table, a membership test
-- ordered: yes
--@ setup
CREATE TABLE lc_links (id int PRIMARY KEY, set_id int NOT NULL, parent_id int NOT NULL);
INSERT INTO lc_links SELECT p, 4, p FROM generate_series(1, (SELECT count(*) * 85 / 100 FROM dims)) p;
CREATE INDEX lc_links_set ON lc_links (set_id, parent_id);
CREATE TABLE lc_jobs AS SELECT i AS id, 1 + i % (SELECT count(*) FROM dims)::int AS parent_id, 1 + i % 12 AS lookup_id, (ARRAY['scheduled', 'started', 'success', 'error'])[1 + i % 4] AS status,
  timestamp '2025-12-29' + (hashint4(i) & 262143) % 86400 * interval '1 second' AS started_at, CASE WHEN i % 4 >= 2 THEN timestamp '2025-12-30' END AS completed_at FROM generate_series(1, 100000) i;
ALTER TABLE lc_jobs ADD PRIMARY KEY (id);
CREATE INDEX lc_jobs_col ON lc_jobs USING sift_col (parent_id, lookup_id, status, started_at, completed_at);
VACUUM (FREEZE, ANALYZE) lc_jobs;
ANALYZE lc_links
--@ pg
SELECT j.lookup_id, sum(CASE WHEN j.status = 'scheduled' THEN 1 ELSE 0 END) AS queued, sum(CASE WHEN j.status = 'started' AND j.started_at >= timestamp '2025-12-29 12:00' THEN 1 ELSE 0 END) AS active,
  sum(CASE WHEN j.status = 'started' AND j.started_at < timestamp '2025-12-29 12:00' THEN 1 ELSE 0 END) AS expired
FROM lc_jobs j WHERE j.completed_at IS NULL AND EXISTS (SELECT 1 FROM lc_links l WHERE l.set_id = 4 AND l.parent_id = j.parent_id)
GROUP BY j.lookup_id ORDER BY j.lookup_id
