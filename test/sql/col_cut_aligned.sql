-- The writer of a merge that cuts its output into partitions by their share of the bytes waits for a value of the first key leaf to end, up to 12% of the share more, before it cuts
-- (sift_colk_cut_near): a value that fits is in one segment, and a value bigger than the slack allows for is cut inside, at the first unit past it. The merge reads nothing more for it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '120s';
SET sift.test_col_build_runs_overlap = on;

-- the decision: due at 1000 bytes, slack 12%: a cut that splits a value waits until 1120, nothing waits when the next row is of another value
SELECT held, open, sift._test_col_cut_near(held, 1000, 12, open) AS cut
FROM (VALUES (999), (1000), (1119), (1120), (1121), (5000)) h(held), (VALUES (false), (true)) o(open) ORDER BY held, open;

-- 185 values of about 1,300 rows (a value that the units of 1,024 rows are cut inside, and fits the slack of a partition of 15,000 rows or more) and one of 20% of the rows, bigger than it
CREATE TABLE ca (id int, dim int, day date, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO ca SELECT g, CASE WHEN g % 5 = 0 THEN 0 ELSE 1 + (g::bigint * 7919 % 185)::int END,
       date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int
FROM generate_series(1, 400000) g;
VACUUM (FREEZE, ANALYZE) ca;
SET sift.test_col_segment_kb = 2048;
SET sift.col_unit_rows = 1024;
CREATE INDEX ca_idx ON ca USING sift_col (dim, day, v, w) WITH (key_cut = 'dim, day');
SELECT count(*) AS runs FROM sift.col_segments('ca_idx') WHERE kind = 'immutable';
SELECT sift.col_merge('ca_idx', true) > 0 AS merged;

CREATE TEMP TABLE ca_segs AS SELECT *, lead(lo) OVER (ORDER BY lo, hi) AS next_lo FROM sift._test_col_segment_keys('ca_idx') WHERE lo <= hi;
CREATE TEMP TABLE ca_vals AS SELECT dim, count(*) AS n FROM ca GROUP BY dim;
SELECT count(*) > 10 AS partitions, sum(rows) = 400000 AS all_rows FROM ca_segs;

-- no value of the first leaf that fits the slack is in two segments: 12 of the 185 were split by the cut at the budget
SELECT count(*) AS split_values_that_fit FROM ca_segs s JOIN ca_vals v ON v.dim = s.hi WHERE s.hi >= s.next_lo AND v.n * 100 <= 12 * (SELECT avg(rows) FROM ca_segs);
-- the value of 20% of the rows is bigger: it is cut inside, in as many segments as its rows make, and each is within the budget and its slack (a segment is never one value of the whole)
SELECT count(*) > 2 AS big_value_in_segments, max(rows) < 40000 AS none_holds_the_whole_value FROM ca_segs WHERE lo = 0;
-- a segment holds at most the budget plus the slack and the unit that took it past: 12% and a unit (25 of them) are under 25% of the median
SELECT max(bytes) <= 1.25 * percentile_cont(0.5) WITHIN GROUP (ORDER BY bytes) AS within_the_slack FROM sift.col_segments('ca_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('ca_idx', true);

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT dim, count(*), sum(v) FROM ca WHERE dim IN (0, 14, 29, 100) GROUP BY dim ORDER BY dim;
SET enable_indexscan = off;
SET enable_seqscan = on;
SELECT dim, count(*), sum(v) FROM ca WHERE dim IN (0, 14, 29, 100) GROUP BY dim ORDER BY dim;
DROP TABLE ca;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
