-- An index built with many segments (a small maintenance_work_mem leaves up to 128) is merged to
-- the tiered policy's fixed point by one VACUUM, not by one merge step per VACUUM (8 segments less).
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;
CREATE TABLE cv (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO cv (body) SELECT 'k' || (i % 7) || ' g' || (i % 1000) || ' common word' || (i % 50) FROM generate_series(1, 30000) i;
CREATE INDEX cv_idx ON cv USING sift (body) WITH (build_segments = 60);
SELECT count(*) >= 60 AS built_in_pieces FROM sift.segments('cv_idx');
SELECT count(*) FROM cv WHERE body @@> 'common';
VACUUM cv;
SELECT count(*) <= 32 AS merged_by_one_vacuum FROM sift.segments('cv_idx');
SELECT count(*) FROM cv WHERE body @@> 'common';
SELECT sift.verify('cv_idx', true);
DROP TABLE cv;
DROP EXTENSION pg_sift;
