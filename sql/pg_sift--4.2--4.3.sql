\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.3'" to load this file. \quit

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

-- Connected indexes (docs/COL-CONNECT.md): a sift_col index with the option connect names, for each column that holds a parent key, the sift_col index of that parent table, and that
-- index keeps a journal of the keys that changed. col_connection_state tells the journal of a parent index (its lineage and the records appended so far) and whether a derived value could
-- be vouched now ('ok', or why not: no journal, the log is not complete, a snapshot that cannot be vouched); col_connection_list gives the connections a child index declares and the
-- parent index that serves each; col_connections lists them all with it.
CREATE FUNCTION sift.col_connection_state(parent regclass, OUT lineage int8, OUT records int8, OUT stamp text) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_connection_state' LANGUAGE C STRICT;

COMMENT ON FUNCTION sift.col_connection_state(regclass) IS 'The key journal of a sift_col index that a child index is connected to: its lineage, the records appended so far, and what a stamp taken now says (ok, or why a derived value could not be vouched)';

CREATE FUNCTION sift.col_connection_list(child regclass, OUT ordinal int4, OUT fk text, OUT declaration text, OUT parent_index regclass, OUT automatic boolean) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_connection_list' LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION sift.col_connection_list(regclass) IS 'The connections a sift_col index declares with its option connect: their place in the option, the column that holds the parent key, the text, the parent index that serves each (NULL when the names do not resolve now) and whether CREATE INDEX or REINDEX chose the connection from the foreign keys';

CREATE FUNCTION sift.col_connection_fill(child regclass, OUT ordinal int4, OUT segments int8, OUT filled int8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_connection_fill' LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION sift.col_connection_fill(regclass) IS 'For each connection of a sift_col index: its segments, and those that hold the found flags under a stamp';

CREATE VIEW sift.col_connections AS
	SELECT c.oid::regclass AS child, l.ordinal, l.fk, l.parent_index AS parent, l.declaration, l.automatic, s.lineage, s.records, s.stamp,
		CASE WHEN f.filled = f.segments THEN 'filled'
			WHEN f.filled = 0 THEN 'unfilled'
			ELSE 'partly filled (' || f.filled || ' of ' || f.segments || ' segments)' END AS fill
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.col_connection_list(c.oid) l
	LEFT JOIN LATERAL sift.col_connection_state(l.parent_index) s ON true
	LEFT JOIN LATERAL sift.col_connection_fill(c.oid) f ON f.ordinal = l.ordinal
	WHERE c.relkind = 'i' AND has_table_privilege(i.indrelid, 'SELECT');

COMMENT ON VIEW sift.col_connections IS 'The connections of the sift_col indexes with the option connect: the child index, the parent index that serves each, the text, the state of the key journal of the parent, and whether the found flags of its segments are filled (fill)';

-- Keys of the primary key that have more than one row in a table (sift.table_find_stale_versions). Releases 4.0.0 to 4.2.1 could make the old versions of updated rows live again
-- after a VACUUM (fixed in 4.2.2); a primary key is then found on more than one row. The table is read by a sequential scan that the aggregate engine and the primary key index
-- cannot answer, which would hide the copies. A row that was deleted and came back has no copy: this function cannot find it.
CREATE OR REPLACE FUNCTION sift.table_find_stale_versions(tbl regclass, sample integer DEFAULT 10)
	RETURNS TABLE (duplicate_keys bigint, sample_keys text[])
	LANGUAGE plpgsql STABLE
	SET sift.enable_agg = off SET enable_indexscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off
AS $$
DECLARE
	cols text;
BEGIN
	SELECT string_agg(format('%I', a.attname), ', ' ORDER BY k.ord) INTO cols
	FROM pg_index i
	CROSS JOIN LATERAL unnest(i.indkey::int2[]) WITH ORDINALITY k(attnum, ord)
	JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum
	WHERE i.indrelid = tbl AND i.indisprimary;
	IF cols IS NULL THEN
		RAISE EXCEPTION 'table % has no primary key', tbl USING ERRCODE = 'undefined_object';
	END IF;
	RETURN QUERY EXECUTE format(
		'SELECT count(*), (array_agg(k ORDER BY k))[1:%s] FROM (SELECT ROW(%s)::text AS k FROM %s GROUP BY %s HAVING count(*) > 1) d',
		sample, cols, tbl, cols);
END
$$;

COMMENT ON FUNCTION sift.table_find_stale_versions(regclass, integer) IS 'The number of primary key values of the table that are on more than one row (the old versions of updated rows that a VACUUM of 4.0.0 to 4.2.1 made live again) and up to sample of them; a row that was deleted and came back has no copy and is not found';

-- The arena of the write buffers (sift.col_wbuf_ref): the rows of the buffers that are references, in shared memory. slots_in_use of slots, the bytes and rows it holds, the rows that found no room
-- (put_fallbacks: written with their values, as without the setting), the buffers asked back for a slot (seals_forced), and the rows built again from the heap since the server started
-- (rows_rebuilt: after a restart, on a standby).
CREATE FUNCTION sift.col_arena_status(OUT slots_in_use int4, OUT slots int4, OUT bytes_in_use int8, OUT rows_stored int8, OUT put_fallbacks int8, OUT seals_forced int8, OUT rows_rebuilt int8, OUT segments int4, OUT bytes_taken int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_arena_status' LANGUAGE C STRICT STABLE;

COMMENT ON FUNCTION sift.col_arena_status() IS 'The shared arena of the write buffers kept as references (sift.col_wbuf_ref): slots in use, bytes and rows held, rows written with their values for want of room, buffers sealed to give a slot back, rows built again from the heap since the server started, the 1 MB segments of shared memory made so far (they stay until the server stops) and the bytes of them that buffers hold';
