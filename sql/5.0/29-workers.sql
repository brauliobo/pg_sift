-- The background workers of sift, as an operator reads them: when the last step of an index ended well, which job it was and how long it took, and the failures noted since
-- the index was built. last_error, failed_at and error_code of sift.worker_state stay after a later step that ended well; the failures in a row (and so next_retry) are what the success clears.
CREATE FUNCTION sift.worker_history(index regclass, OUT last_ok timestamptz, OUT last_job text, OUT last_ms int8, OUT failures_total int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_worker_history' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.worker_history(regclass) IS 'When the last background step of a sift or sift_col index that ended well ended, its job (promote, merge, rewrite, vacuum, track or rollup) and its time in ms, and the failures noted for the index so far. Read through sift.workers.';

-- An index that is out of service is listed with what the registry knows of it and nothing read from its pages: the view that is read to find the damaged indexes must not fail on one.
CREATE OR REPLACE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(r.oid) END AS due,
		   c.oid::regclass AS relation, i.indrelid::regclass AS "table",
		   h.last_ok, h.last_job, h.last_ms, h.failures_total,
		   qs.quarantined, qs.since AS quarantined_since, qs.reason AS quarantine_reason
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift._quarantine_state(c.oid) qs
	LEFT JOIN LATERAL (SELECT c.oid WHERE NOT qs.quarantined) r ON true
	LEFT JOIN LATERAL sift.worker_state(r.oid) w ON true
	LEFT JOIN LATERAL sift.worker_history(r.oid) h ON true
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid) AND (w.segments IS NOT NULL OR qs.quarantined)
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error, NULL::text, c.oid::regclass, c.oid::regclass,
		   NULL::timestamptz, NULL::text, NULL::int8, NULL::int8, false, NULL::timestamptz, NULL::text
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind IN ('r', 'm') AND sift._may_inspect(c.oid) AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index and per sift_table the caller can look at: segments, write buffers, whether maintenance runs, the failures in a row, the next retry, the last error, why a sift_col index would be rewritten, the last step that ended well, and whether the index is out of service (quarantined, since when and why).';

-- The maintenance jobs that run now, one row each: a rewrite, a merge, a promotion, the merges of a VACUUM, whether a worker or a session runs them. The slots are in shared memory (nothing on disk) and a job is listed while it runs.
CREATE FUNCTION sift._progress(OUT pid int4, OUT index oid, OUT job text, OUT phase text, OUT segments_done int8, OUT segments_total int8, OUT bytes_written int8, OUT started timestamptz)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_progress_rows' LANGUAGE C STABLE;

CREATE VIEW sift.progress AS
	SELECT p.pid, p.index::regclass AS index, p.job, p.phase, p.segments_done, p.segments_total, p.bytes_written, p.started
	FROM sift._progress() p
	JOIN pg_index i ON i.indexrelid = p.index
	WHERE sift._may_inspect(i.indrelid);
COMMENT ON VIEW sift.progress IS 'The maintenance jobs of the database that run now on an index the caller can look at: the process (pid), the job (promote, merge, rewrite or vacuum), its phase, the segments it has replaced of those the phase found, the bytes it wrote and when it started.';
GRANT SELECT ON sift.progress TO PUBLIC;

-- Why the last cut of the file of a sift_col index left free blocks (shrink_blocked_by: standby, lock or snapshot, NULL when nothing blocks it) and when the file was last cut
CREATE FUNCTION sift.col_shrink_history(index regclass, OUT shrink_blocked_by text, OUT last_shrink timestamptz)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_shrink_history' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.col_shrink_history(regclass) IS 'Why the last cut of the file of a sift_col index left free blocks (standby: a standby streams; lock: a lock on the index was held; snapshot: a snapshot may still read the blocks; NULL: nothing blocks it) and when the file was last cut. Read next to sift.col_space.';
