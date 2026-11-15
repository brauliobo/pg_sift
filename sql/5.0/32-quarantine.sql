-- A sift or sift_col index that finds its own damage leaves service (sift.on_corruption, docs/OPERATIONS.md): statements run on the heap until REINDEX rebuilds it.
-- sift._quarantine() is the registry of the server for the files of this database; sift.quarantine lists the indexes that are out of service now.
CREATE FUNCTION sift._quarantine(OUT index oid, OUT filenode oid, OUT detected_at timestamptz, OUT confirmed bool, OUT persisted bool, OUT error_code text, OUT reason text,
								 OUT detections int8, OUT skipped_writes int8, OUT skipped_vacuums int8, OUT refused_scans int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_quarantine_rows' LANGUAGE C STABLE;

CREATE VIEW sift.quarantine AS
	SELECT q.index::regclass AS index, q.detected_at, q.persisted, q.error_code, q.reason, q.detections, q.skipped_writes, q.skipped_vacuums, q.refused_scans
	FROM sift._quarantine() q
	JOIN pg_class c ON c.oid = q.index AND pg_relation_filenode(c.oid) = q.filenode
	JOIN pg_index i ON i.indexrelid = c.oid
	WHERE q.confirmed AND sift._may_inspect(i.indrelid);
COMMENT ON VIEW sift.quarantine IS 'The sift and sift_col indexes of the database that are out of service because they found their own damage: when, the SQLSTATE and the first error text, how many times the damage was seen and what was skipped since (writes, vacuums and scans that did not use the index). REINDEX puts an index back in service.';
