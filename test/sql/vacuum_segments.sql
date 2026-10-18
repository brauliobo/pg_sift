-- vacuum_segments: the directory entries VACUUM merges an index down to (default 2), changed with ALTER INDEX and no REINDEX.
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;
CREATE TABLE vs (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO vs (body) SELECT 'k' || (i % 7) || ' g' || (i % 1000) || ' common word' || (i % 50) FROM generate_series(1, 30000) i;
-- built without the option: the default
CREATE INDEX vs_idx ON vs USING sift (body) WITH (build_segments = 60);
SELECT count(*) AS built FROM sift.segments('vs_idx');
SELECT count(*) AS hits FROM vs WHERE body @@> 'common';
VACUUM vs;
SELECT count(*) AS after_default FROM sift.segments('vs_idx');
SELECT count(*) AS hits FROM vs WHERE body @@> 'common';

-- a higher value: less merging, more segments
DROP INDEX vs_idx;
CREATE INDEX vs_idx ON vs USING sift (body) WITH (build_segments = 60);
ALTER INDEX vs_idx SET (vacuum_segments = 4);
SELECT sift.verify('vs_idx');
VACUUM vs;
SELECT count(*) AS after_4 FROM sift.segments('vs_idx');
SELECT count(*) AS hits FROM vs WHERE body @@> 'common';
SELECT sift.verify('vs_idx', true);

-- lowered to 1, VACUUM merges further; raised above the segments it has: a no-op
ALTER INDEX vs_idx SET (vacuum_segments = 1);
VACUUM vs;
SELECT count(*) AS after_1 FROM sift.segments('vs_idx');
CREATE TEMP TABLE before AS SELECT gen FROM sift.segments('vs_idx');
ALTER INDEX vs_idx SET (vacuum_segments = 128);
VACUUM vs;
SELECT count(*) AS unchanged FROM sift.segments('vs_idx') s JOIN before b USING (gen);
SELECT sift.verify('vs_idx', true);

-- out of range
ALTER INDEX vs_idx SET (vacuum_segments = 0);
ALTER INDEX vs_idx SET (vacuum_segments = 129);
ALTER INDEX vs_idx RESET (vacuum_segments);
SELECT sift.verify('vs_idx');
DROP TABLE vs;
DROP EXTENSION pg_sift;
