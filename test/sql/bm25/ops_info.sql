-- what an operator sees: sift.version(), sift.index_info(), the extents and dead documents of sift.segments()
CREATE EXTENSION pg_sift;

SELECT sift.version() = extversion AS library_matches_extension FROM pg_extension WHERE extname = 'pg_sift';

CREATE TABLE oi (id int, body text);
INSERT INTO oi SELECT i, 'alpha beta ' || i FROM generate_series(1, 3000) i;
CREATE INDEX oi_idx ON oi USING sift (body);

SELECT relation_blocks = high_water AS blocks_in_use, free_blocks, reusable_blocks, free_extents, segments, max_segments,
	ndocs, maintenance_busy
FROM sift.index_info('oi_idx');
SELECT kind, ndocs, extents, dead_docs FROM sift.segments('oi_idx');

-- VACUUM marks the deleted documents dead; merging the segment drops them and frees its blocks
DELETE FROM oi WHERE id <= 300;
VACUUM oi;
SELECT kind, ndocs, extents, dead_docs FROM sift.segments('oi_idx');
SELECT sift.merge('oi_idx', true);
SELECT kind, ndocs, dead_docs FROM sift.segments('oi_idx');
SELECT free_blocks > 0 AS blocks_freed, reusable_blocks <= free_blocks AS reusable_within_free, ndocs
FROM sift.index_info('oi_idx');

-- a write buffer has no extents and no dead documents to report
INSERT INTO oi VALUES (5000, 'gamma');
SELECT kind, extents IS NULL AS no_extents, dead_docs IS NULL AS no_dead FROM sift.segments('oi_idx') ORDER BY gen;

DROP TABLE oi;
DROP EXTENSION pg_sift;
