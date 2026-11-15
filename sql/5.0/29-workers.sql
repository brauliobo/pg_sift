-- The background workers of sift, as an operator reads them: when the last step of an index ended well, which job it was and how long it took, and the failures noted since
-- the index was built. last_error, failed_at and error_code of sift.worker_state stay after a later step that ended well; the failures in a row (and so next_retry) are what the success clears.
CREATE FUNCTION sift.worker_history(index regclass, OUT last_ok timestamptz, OUT last_job text, OUT last_ms int8, OUT failures_total int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_worker_history' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.worker_history(regclass) IS 'When the last background step of a sift or sift_col index that ended well ended, its job (promote, merge, rewrite, vacuum, track or rollup) and its time in ms, and the failures noted for the index so far. Read through sift.workers.';

CREATE OR REPLACE VIEW sift.workers AS
	SELECT c.oid::regclass AS index, a.amname::text AS kind, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error,
		   CASE WHEN a.amname = 'sift_col' THEN sift.col_maintenance_due(c.oid) END AS due,
		   c.oid::regclass AS relation, i.indrelid::regclass AS "table",
		   h.last_ok, h.last_job, h.last_ms, h.failures_total
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname IN ('sift', 'sift_col')
	JOIN pg_index i ON i.indexrelid = c.oid AND i.indisready
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	CROSS JOIN LATERAL sift.worker_history(c.oid) h
	WHERE c.relkind = 'i' AND sift._may_inspect(i.indrelid) AND w.segments IS NOT NULL
	UNION ALL
	SELECT c.oid::regclass, a.amname::text, w.segments, w.sealed, w.running, w.failures, w.failed_at, w.next_retry,
		   w.error_code, w.last_error, NULL::text, c.oid::regclass, c.oid::regclass,
		   NULL::timestamptz, NULL::text, NULL::int8, NULL::int8
	FROM pg_class c
	JOIN pg_am a ON a.oid = c.relam AND a.amname = 'sift_table'
	CROSS JOIN LATERAL sift.worker_state(c.oid) w
	WHERE c.relkind IN ('r', 'm') AND sift._may_inspect(c.oid) AND w.segments IS NOT NULL;
COMMENT ON VIEW sift.workers IS 'One row per sift and sift_col index and per sift_table the caller can look at: segments, write buffers, whether maintenance runs, the failures in a row, the next retry, the last error (kept after a later success), why a sift_col index would be rewritten, the last step that ended well and the failures noted so far.';
