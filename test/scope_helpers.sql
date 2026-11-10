\set ECHO none
-- Shared by the scope index tests (\ir test/scope_helpers.sql; the fuzz test/fuzz/col_scope_fuzz.py sends this file to its sessions): the statement of a scope read against the plain join, and
-- the reasons the scan of a scope may give for reading the entity rows.
--
-- scope_reason_ok(why): whether a "Scope Fallback" reason is one of the documented ones (docs/COL-SCOPE.md).
-- scope_run(q, plain): the scope form q and the plain form `plain` of one statement: same rows through the scope path, the planned plan and the plain plan (sift.enable_agg off), whether the
--   scope units were read (stored), the fallback reason (why), the patched members (patched), whether the scan has a scope conjunct at all (engaged), and whether the path is accounted for (path_ok: stored, or a documented reason).
CREATE FUNCTION scope_reason_ok(why text) RETURNS boolean LANGUAGE sql IMMUTABLE AS $$
	SELECT why ~ '^(disabled|no scope index|column .* not in the scope units|unfilled|several stamps|scope segments do not cover the rows|write buffer|top-n, changed pairs|'
		'dedupe over work_mem|dedupe keys|no journal|log not complete|other lineage|trimmed|snapshot|snapshot older than the stamp|stamp aged|too many changed keys|per pair, changed pairs)$'
$$;

CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT why text, OUT patched boolean, OUT engaged boolean, OUT path_ok boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	c text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.agg_force = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO c;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || plain || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	same := a = b AND c = b;
	why := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	patched := jsonb_path_exists(p, '$.**."Scope Patched Keys"');
	engaged := jsonb_path_exists(p, '$.**."Scope Index"');
	path_ok := stored OR scope_reason_ok(why);
END $$;
\set ECHO all
