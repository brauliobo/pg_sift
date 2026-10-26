\echo Use "ALTER EXTENSION pg_sift UPDATE TO '3.5'" to load this file. \quit

-- 3.5 adds one SQL object, sift.json_build_object (below); the library is new (the planner, the column format minor 22). The update also moves the version, so that
-- sift.version() and the catalog agree and ALTER EXTENSION UPDATE is the one step of every upgrade.

-- json_build_object with the type lookups of each argument made once per call site, not per call; the same text (test json_build_object). Put sift before pg_catalog in the search_path to take it for every call of the name.
CREATE FUNCTION sift.json_build_object(VARIADIC "any") RETURNS json
	AS 'MODULE_PATHNAME', 'sift_json_build_object' LANGUAGE C STABLE PARALLEL SAFE;
COMMENT ON FUNCTION sift.json_build_object("any") IS 'json_build_object with the type lookups of each argument made once per call site: the same text as the core function, for one or more arguments';
