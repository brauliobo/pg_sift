-- The registry of the indexes that left service because they found their own damage (docs/OPERATIONS.md), read by the views that come before sift.quarantine (sift.workers, sift.col_index_status).
-- sift._quarantine() is the registry of the server for the files of this database; sift._meta_quarantined(index) is the flag of the metapage.
CREATE FUNCTION sift._quarantine(OUT index oid, OUT filenode oid, OUT detected_at timestamptz, OUT confirmed bool, OUT persisted bool, OUT error_code text, OUT reason text,
								 OUT detections int8, OUT skipped_writes int8, OUT skipped_vacuums int8, OUT refused_scans int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_quarantine_rows' LANGUAGE C STABLE;

CREATE FUNCTION sift._meta_quarantined(index oid) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_meta_quarantined' LANGUAGE C STABLE STRICT;

-- Whether the index is out of service, since when (NULL for an index that only its metapage flags) and why
CREATE FUNCTION sift._quarantine_state(index oid, OUT quarantined bool, OUT since timestamptz, OUT reason text)
	RETURNS record LANGUAGE sql STABLE AS $$
	SELECT r.detected_at IS NOT NULL OR f.flagged, r.detected_at,
		   CASE WHEN r.detected_at IS NOT NULL THEN r.reason WHEN f.flagged THEN 'flagged out of service in its metapage' END
	FROM (SELECT sift._meta_quarantined($1) AS flagged) f
	LEFT JOIN (SELECT q.detected_at, q.reason FROM sift._quarantine() q WHERE q.index = $1 AND q.confirmed AND q.filenode = pg_relation_filenode($1)) r ON true
$$;
