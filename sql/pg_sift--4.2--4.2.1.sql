\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.2.1'" to load this file. \quit

-- The two triggers of an index with track_changes were its dependents, and DROP INDEX CONCURRENTLY refused such an index (it deletes what depends on the index first, which assigns
-- the transaction an xid that the command does not allow). The index now depends on its triggers and the transaction that deletes it removes them. This function makes the
-- existing indexes so (a second run finds nothing to convert); its presence is what tells the library that it was done: until then the library does not find the triggers of an index
-- made by an earlier release (its reads check the heap) and removes no trigger of an index that goes (the triggers of an index it made itself have the new dependency from the start).
-- The script of the next minor from 4.2 must create it as well (CREATE OR REPLACE: a patch release moves the extension version back to 4.2 and forward again).
CREATE OR REPLACE FUNCTION sift.col_track_convert() RETURNS integer
	AS 'MODULE_PATHNAME', 'sift_col_track_convert' LANGUAGE C STRICT;

COMMENT ON FUNCTION sift.col_track_convert() IS 'Makes each sift_col index the dependent of its two track_changes triggers (what 4.2.1 does for the indexes of an older release), so that DROP INDEX CONCURRENTLY can drop it; returns the number of triggers converted';

SELECT sift.col_track_convert();

-- sift.workers and sift.col_index_status read the metapage of every sift index of the database, and an index that CREATE INDEX CONCURRENTLY or REINDEX CONCURRENTLY is still building (or a failed one left) has none:
-- one such index made both views fail for the whole database, on the primary and, replaying the build, on a standby ("could not read blocks", "invalid page layout"). They list the indexes that are ready.
CREATE OR REPLACE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind = 'i' AND has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error, NULL::text
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind IN ('r', 'm') AND has_table_privilege(c.oid, 'SELECT') AND w.segments IS NOT NULL;

CREATE OR REPLACE VIEW sift.col_index_status AS
	SELECT c.oid::regclass AS index, m.heap AS "table",
		   pg_relation_size(c.oid) AS index_bytes, x.bytes AS segment_bytes,
		   x.free_blocks * bs.n AS free_bytes, x.reusable_blocks * bs.n AS reusable_bytes,
		   x.rows AS segment_rows, x.segments, x.max_segments, s.largest_segment_bytes, x.units, s.listed_units,
		   s.dirty_segments, s.dead_rows, round(100.0 * s.dead_rows / NULLIF(x.rows, 0), 2) AS dead_pct,
		   s.buffer_rows, w.sealed AS sealed_buffers, w.running, w.failures, w.next_retry,
		   m.last_vacuum, m.changed_rows, m.vacuum_at, m.rewrite_due AS due, m.last_request, m.blocked_by
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN (SELECT current_setting('block_size')::int8 AS n) bs
	CROSS JOIN LATERAL sift.col_index_info(c.oid) x
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	CROSS JOIN LATERAL sift.col_maintenance_status(c.oid) m
	CROSS JOIN LATERAL (SELECT max(bytes) FILTER (WHERE kind = 'immutable') AS largest_segment_bytes,
							   sum(units) AS listed_units, count(*) FILTER (WHERE dirty) AS dirty_segments,
							   coalesce(sum(dead_rows), 0) AS dead_rows,
							   coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) AS buffer_rows
						FROM sift.col_segments(c.oid)) s
	WHERE has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL;
