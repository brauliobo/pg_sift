
-- The format an index is in, from the installed SQL: sift.index_format (the metapage of any of the three access methods; the preflight and sift.upgrade_commands() read it) and the format columns of
-- sift.col_index_status. Before 5.0 the function existed in the library only, and the preflight declared it inside a transaction that it rolled back.
CREATE FUNCTION sift.index_format(rel regclass, OUT major int4, OUT minor int4, OUT long_words bool, OUT reads text) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_index_format' LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION sift.index_format(regclass) IS 'The metapage format of a sift index, a sift_col index or a sift_table: major and minor; long_words, for a sift index, tells whether it was built by 5.0 or later, which stores a word over 255 bytes as a prefix and a hash (a REINDEX marks an older one); reads, for the other two, the column formats this library reads (e.g. 3 to 51)';

CREATE OR REPLACE VIEW sift.col_index_status AS
	SELECT c.oid::regclass AS index, m.heap AS "table",
		   pg_relation_size(c.oid) AS index_bytes, x.bytes AS segment_bytes,
		   x.free_blocks * bs.n AS free_bytes, x.reusable_blocks * bs.n AS reusable_bytes,
		   x.rows AS segment_rows, x.segments, x.max_segments, s.largest_segment_bytes, x.units, s.listed_units,
		   s.dirty_segments, s.dead_rows, round(100.0 * s.dead_rows / NULLIF(x.rows, 0), 2) AS dead_pct,
		   s.buffer_rows, w.sealed AS sealed_buffers, w.running, w.failures, w.next_retry,
		   m.last_vacuum, m.changed_rows, m.vacuum_at, m.rewrite_due AS due, m.last_request, m.blocked_by,
		   f.major || '.' || f.minor AS format, s.min_segment_format, s.max_segment_format, f.reads AS library_reads
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_col'
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN (SELECT current_setting('block_size')::int8 AS n) bs
	CROSS JOIN LATERAL sift.col_index_info(c.oid) x
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	CROSS JOIN LATERAL sift.col_maintenance_status(c.oid) m
	CROSS JOIN LATERAL sift.index_format(c.oid) f
	CROSS JOIN LATERAL (SELECT max(bytes) FILTER (WHERE kind = 'immutable') AS largest_segment_bytes,
							   sum(units) AS listed_units, count(*) FILTER (WHERE dirty) AS dirty_segments,
							   coalesce(sum(dead_rows), 0) AS dead_rows,
							   coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) AS buffer_rows,
							   min(format) AS min_segment_format, max(format) AS max_segment_format
						FROM sift.col_segments(c.oid)) s
	WHERE has_table_privilege(i.indrelid, 'SELECT') AND w.segments IS NOT NULL;

COMMENT ON VIEW sift.col_index_status IS 'One row per sift_col index the caller can read, for the DBA of a large table: sizes, segments against the limit, rows, dirty segments and dead rows, write buffers, the maintenance state and what blocks a rewrite (sift.col_maintenance_status), and the format: the metapage, the oldest and newest column format of its segments, and the range this library reads.';
