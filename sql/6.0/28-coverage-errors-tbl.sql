-- sift._tbl_rmgr_registered(): whether this server registered the resource managers of sift_table, which only a library that was preloaded does. A published sift_table
-- needs them (docs/COL-TAM.md section 5), so the check of a publication asks the server, not the text of shared_preload_libraries, which names the library by a path, a version
-- or a link as well as by its name.
CREATE OR REPLACE FUNCTION sift._tbl_rmgr_registered() RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_tbl_rmgr_registered' LANGUAGE C STABLE PARALLEL SAFE;
COMMENT ON FUNCTION sift._tbl_rmgr_registered() IS 'Internal: whether the server registered the pg_sift resource managers, which only a preloaded library does. What sift.publication_check asks.';

CREATE OR REPLACE FUNCTION sift.publication_check() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
DECLARE
	tbl regclass;
BEGIN
	IF current_setting('wal_level') <> 'logical' OR sift._tbl_rmgr_registered() THEN
		RETURN;
	END IF;
	SELECT g.relid::regclass INTO tbl
		FROM pg_event_trigger_ddl_commands() d
			JOIN pg_publication p ON p.oid = CASE d.classid
					WHEN 'pg_publication'::regclass THEN d.objid
					WHEN 'pg_publication_rel'::regclass THEN (SELECT r.prpubid FROM pg_publication_rel r WHERE r.oid = d.objid)
					WHEN 'pg_publication_namespace'::regclass THEN (SELECT n.pnpubid FROM pg_publication_namespace n WHERE n.oid = d.objid)
				END
			CROSS JOIN LATERAL pg_get_publication_tables(p.pubname) g
			JOIN pg_class c ON c.oid = g.relid
			JOIN pg_am a ON a.oid = c.relam
		WHERE a.amname = 'sift_table' AND c.relpersistence = 'p' AND (p.pubinsert OR p.pubupdate OR p.pubdelete)
		LIMIT 1;
	IF tbl IS NOT NULL THEN
		RAISE EXCEPTION 'sift_table table "%" is published and wal_level is "logical", but pg_sift is not in shared_preload_libraries', tbl
			USING ERRCODE = 'feature_not_supported',
				DETAIL = 'Logical replication of a sift_table table needs the pg_sift resource manager, which is registered only when the library is preloaded.',
				HINT = format('Add pg_sift to shared_preload_libraries (primary and every standby), for example ALTER SYSTEM SET shared_preload_libraries = ''pg_sift'' with the libraries it lists now, and restart, or publish a heap staging table instead of "%s".', tbl);
	END IF;
END
$fn$;
