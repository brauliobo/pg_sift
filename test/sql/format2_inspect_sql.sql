-- The functions that read or drive the file of an index (sift.col_space, sift.col_extents, sift.col_index_info, sift.col_segments, sift.col_leaves, sift.col_verify, sift.col_maintain,
-- sift.index_format, sift.worker_state and the views over them) answer for an index of format major 2 from its metapage, allocator and manifest, as they do for one of format major 1.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE ev (id int, grp int, amount numeric(10, 2), tag text);
INSERT INTO ev SELECT i, i % 7, (i % 1000) / 10.0, 'tag-' || (i % 50) FROM generate_series(1, 50000) i;
CREATE INDEX ev_idx ON ev USING sift_col (id, grp, amount, tag) WITH (key_order = 'id');

SELECT major, minor FROM sift.index_format('ev_idx');
SELECT relation_blocks > 0 AS has_blocks, high_water > 0 AS has_high_water, used_blocks > 0 AS has_used FROM sift.col_space('ev_idx');
SELECT count(*) > 0 AS has_extents, sum(nblocks) <= (SELECT high_water FROM sift.col_space('ev_idx')) AS extents_in_file FROM sift.col_extents('ev_idx');
SELECT rows, segments >= 1 AS has_parts, leaves FROM sift.col_index_info('ev_idx');
SELECT count(*) >= 1 AS has_parts, sum(rows) AS rows, bool_and(kind = 'immutable') AS immutable FROM sift.col_segments('ev_idx');
SELECT leaf, type FROM sift.col_leaves('ev_idx') ORDER BY leaf;
SELECT sift.col_verify('ev_idx');
SELECT index, segments >= 1 AS has_parts, segment_rows FROM sift.col_index_status WHERE index = 'ev_idx'::regclass;
SELECT segments >= 1 AS has_parts, sealed, failures FROM sift.worker_state('ev_idx');
SELECT heap, blocked_by FROM sift.col_maintenance_status('ev_idx');

DELETE FROM ev WHERE id % 2 = 0;
SELECT sift.col_maintain('ev_idx');
SELECT sift.col_verify('ev_idx');
SELECT rows FROM sift.col_index_info('ev_idx');
DROP TABLE ev;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
