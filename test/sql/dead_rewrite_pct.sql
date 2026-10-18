-- dead_rewrite_pct: the percent of dead documents above which maintenance rewrites a lone segment (default 50), changed with ALTER INDEX and no REINDEX.
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;
SET sift.async_maintenance = off;
CREATE TABLE dr (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO dr (body) SELECT 'k' || (i % 7) || ' g' || (i % 1000) || ' common' FROM generate_series(1, 10000) i;
CREATE INDEX dr_idx ON dr USING sift (body) WITH (build_segments = 1);
CREATE TEMP TABLE built AS SELECT gen FROM sift.segments('dr_idx');
SELECT count(*) AS segments FROM built;

-- 30% dead: under the default 50 the segment stays
DELETE FROM dr WHERE id % 10 < 3;
VACUUM dr;
SELECT count(*) AS kept_at_default FROM sift.segments('dr_idx') s JOIN built USING (gen);

-- the same 30% with the threshold at 25: VACUUM rewrites it and drops the dead documents
ALTER INDEX dr_idx SET (dead_rewrite_pct = 25);
VACUUM dr;
SELECT count(*) AS kept_at_25 FROM sift.segments('dr_idx') s JOIN built USING (gen);
SELECT sum(ndocs) AS docs FROM sift.segments('dr_idx');
SELECT count(*) AS hits FROM dr WHERE body @@> 'common';
SELECT sift.verify('dr_idx', true);

-- 100 never rewrites, 1 rewrites at the first dead document
DELETE FROM dr WHERE id % 10 IN (3, 4);
CREATE TEMP TABLE before AS SELECT gen FROM sift.segments('dr_idx');
ALTER INDEX dr_idx SET (dead_rewrite_pct = 100);
VACUUM dr;
SELECT count(*) AS kept_at_100 FROM sift.segments('dr_idx') JOIN before USING (gen);
ALTER INDEX dr_idx SET (dead_rewrite_pct = 1);
VACUUM dr;
SELECT count(*) AS kept_at_1 FROM sift.segments('dr_idx') JOIN before USING (gen);
SELECT count(*) AS hits FROM dr WHERE body @@> 'common';
SELECT sift.verify('dr_idx', true);

-- out of range
ALTER INDEX dr_idx SET (dead_rewrite_pct = 0);
ALTER INDEX dr_idx SET (dead_rewrite_pct = 101);
ALTER INDEX dr_idx RESET (dead_rewrite_pct);
SELECT sift.verify('dr_idx');
DROP TABLE dr;
DROP EXTENSION pg_sift;
