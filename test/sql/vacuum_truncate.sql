-- VACUUM gives the free blocks at the end of the index file back to the operating system (DESIGN.md (i)), like it does for a
-- heap: after a bulk delete, the merge that rewrites the surviving documents into the space in front of the old segments and the
-- VACUUM that follows. vacuum_truncate = false keeps the file as it is.
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;
CREATE FUNCTION blocks(idx regclass) RETURNS bigint LANGUAGE sql
	AS $$ SELECT relation_blocks FROM sift.index_info(idx) $$;
-- the file ends where the store says
CREATE FUNCTION sized(idx regclass) RETURNS bool LANGUAGE sql
	AS $$ SELECT relation_blocks = high_water FROM sift.index_info(idx) $$;

CREATE TABLE vt (id int, body text) WITH (autovacuum_enabled = off);
INSERT INTO vt SELECT g, 'k' || (g % 7) || ' g' || (g % 1000) || ' common word' || (g % 50) || ' u' || g FROM generate_series(1, 100000) g;
CREATE INDEX vt_idx ON vt USING sift (body) WITH (build_segments = 8);
SELECT blocks('vt_idx') AS built \gset
DELETE FROM vt WHERE id % 20 <> 0;
-- one VACUUM: merged down to the two segments of vacuum_segments, which are written into the space of the old ones (the VACUUM holds the
-- index exclusively then, so what it releases is reusable at once) and the end of the file is cut
VACUUM vt;
SELECT blocks('vt_idx') < :built / 4 AS shrunk, free_blocks * 10 < relation_blocks AS little_free, sized('vt_idx') FROM sift.index_info('vt_idx');
SELECT sift.verify('vt_idx', true);
SELECT count(*) AS hits FROM vt WHERE body @@> 'common';
-- nothing more to give back
SELECT blocks('vt_idx') AS settled \gset
VACUUM vt;
SELECT blocks('vt_idx') <= :settled AS no_growth;

-- the file grows again as needed, and a session that appended before the truncation appends after it
INSERT INTO vt SELECT g, 'k' || (g % 7) || ' fresh common u' || g FROM generate_series(200001, 230000) g;
SELECT count(*) AS hits FROM vt WHERE body @@> 'fresh';
SELECT sift.promote('vt_idx') >= 0 AS promoted;
SELECT sift.verify('vt_idx', true);
SELECT sized('vt_idx');

-- vacuum_truncate = false: VACUUM leaves the file alone, the next one with the default gives the end back
CREATE TABLE vt2 (id int, body text) WITH (autovacuum_enabled = off);
INSERT INTO vt2 SELECT g, 'k' || (g % 7) || ' g' || (g % 1000) || ' common word' || (g % 50) || ' u' || g FROM generate_series(1, 100000) g;
CREATE INDEX vt2_idx ON vt2 USING sift (body) WITH (build_segments = 8, vacuum_truncate = false);
DELETE FROM vt2 WHERE id % 20 <> 0;
VACUUM vt2;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
SELECT sift.merge('vt2_idx', true);
SELECT blocks('vt2_idx') AS kept \gset
VACUUM vt2;
SELECT blocks('vt2_idx') = :kept AS file_kept, free_blocks > :kept / 2 AS mostly_free FROM sift.index_info('vt2_idx');
ALTER INDEX vt2_idx RESET (vacuum_truncate);
-- a sift index is not vacuumed by workers, so a VACUUM that has one btree besides it is not in parallel mode. With two the leader
-- runs the index cleanup in parallel mode, where the exclusive lock cannot be taken (it needs a transaction id): it does not truncate
-- and does not fail; autovacuum and a VACUUM without workers do
CREATE INDEX vt2_id ON vt2 (id);
CREATE INDEX vt2_id2 ON vt2 (id, body);
SET min_parallel_index_scan_size = 0;
VACUUM (PARALLEL 2) vt2;
RESET min_parallel_index_scan_size;
SELECT blocks('vt2_idx') = :kept AS parallel_keeps;
VACUUM (PARALLEL 0) vt2;
SELECT blocks('vt2_idx') < :kept / 4 AS truncated, sized('vt2_idx');
SELECT sift.verify('vt2_idx', true);
SELECT count(*) AS hits FROM vt2 WHERE body @@> 'common';

-- the setting takes a boolean
ALTER INDEX vt_idx SET (vacuum_truncate = 2);
ALTER INDEX vt_idx SET (vacuum_truncate = on);
ALTER INDEX vt_idx RESET (vacuum_truncate);

DROP TABLE vt, vt2;
DROP FUNCTION blocks(regclass), sized(regclass);
DROP EXTENSION pg_sift;
