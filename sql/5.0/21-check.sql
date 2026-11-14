-- sift.table_verify(table): the structural checks of every column segment of a sift_table (chunks, zones, the cube and rollups against the chunks), one segment at a time. Returns the live rows of
-- the segments; an error on damage, as sift.col_verify and sift.verify for an index. Owner of the table or MAINTAIN, as for VACUUM.
CREATE FUNCTION sift.table_verify(tbl regclass) RETURNS bigint
	AS 'MODULE_PATHNAME', 'sift_table_verify' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.table_verify(regclass) IS 'Verifies the column segments of a sift_table and returns their live rows; an error on damage. Needs MAINTAIN on the table';

-- The problems of one index or table, a row each (the walker of sift.check)
CREATE FUNCTION sift._check(object regclass, heapallindexed bool, max_problems int)
RETURNS TABLE (index regclass, segment bigint, problem text, detail text)
	AS 'MODULE_PATHNAME', 'sift_check_one' LANGUAGE C STRICT VOLATILE;

-- sift.check(index, heapallindexed, max_problems): the verification of sift.verify, sift.col_verify and sift.table_verify as rows. No row: nothing wrong. A row for each segment (and for the totals of the
-- metapage and the rows against the heap: segment NULL) that fails its checks, with what the raising form says; an index with a damaged segment is read to the end, the other segments are checked.
-- index NULL walks every sift, sift_col and sift_table the caller may maintain (owner, MAINTAIN, or pg_maintain); a named one the caller may not maintain is an error. Only damage is reported: a
-- cancel, a serialization failure on a standby (retry) or any other error leaves the function. Holds the maintenance lock of one segment at a time, and works on a standby.
CREATE FUNCTION sift.check(index regclass DEFAULT NULL, heapallindexed bool DEFAULT false, max_problems int DEFAULT 100)
RETURNS TABLE (index regclass, segment bigint, problem text, detail text) LANGUAGE sql VOLATILE AS $$
	-- $1 index, $2 heapallindexed, $3 max_problems (CHECK is a keyword: no qualifying with the function name)
	SELECT r.index, r.segment, r.problem, r.detail
	FROM (SELECT o.oid
		  FROM pg_catalog.pg_class o
		  JOIN pg_catalog.pg_am a ON a.oid = o.relam
		  LEFT JOIN pg_catalog.pg_index i ON i.indexrelid = o.oid
		  WHERE CASE WHEN $1 IS NOT NULL THEN o.oid = $1
				ELSE a.amname IN ('sift', 'sift_col', 'sift_table') AND o.relkind IN ('i', 'r', 'm')
					 AND (i.indexrelid IS NULL OR (i.indisvalid AND i.indisready))
					 AND (o.relpersistence <> 't' OR o.relnamespace = pg_catalog.pg_my_temp_schema())
					 AND (o.relpersistence <> 'u' OR NOT pg_catalog.pg_is_in_recovery())
					 AND pg_catalog.has_table_privilege(coalesce(i.indrelid, o.oid), 'MAINTAIN')
				END
		  ORDER BY o.oid) c,
		 LATERAL sift._check(c.oid, $2, $3) r
	LIMIT $3
$$;
COMMENT ON FUNCTION sift.check(regclass, bool, int) IS 'The problems of the sift indexes and tables the caller may maintain (or of one), a row each with its segment; no row when sound. heapallindexed also compares every heap row with the index. Works on a standby';
