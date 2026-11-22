-- Object storage tiering and TTL of a Format 2.0 relation (a sift_table, or a sift_col index): the two policy tables, the functions that set them and the view that lists the parts in the store.

-- The location a relation's old parts move to, keyed by the OID of the relation so that a rename or a move to another schema keeps it. Not dumped by pg_dump: the objects belong to the physical
-- relation (they are named by scope_id, which pg_dump would copy to a second relation that then overwrites them), so a restored relation has no parts in object storage and gets its policy again
-- from sift.set_tier_policy. cache_mb bounds the local cache of ranges of that policy (a session reads it when it opens the cache); credentials of an S3 or R2 store are never in a table.
CREATE TABLE IF NOT EXISTS sift.tier_policies (
	policy_id	int4 GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	relid		regclass NOT NULL UNIQUE,
	uri			text NOT NULL CHECK (uri ~ '^(file:///|s3://|r2://)[^?#@]*$'),
	move_after	interval NOT NULL CHECK (move_after > interval '0'),
	endpoint	text CHECK (endpoint ~ '^https?://[^?#@]*$'),
	cache_mb	int4 NOT NULL DEFAULT 1024 CHECK (cache_mb BETWEEN 16 AND 1048576),
	scope_id	int8 NOT NULL UNIQUE CHECK (scope_id > 0)
);
COMMENT ON TABLE sift.tier_policies IS 'The object store each sift_table or sift_col index moves its old parts to (set by sift.set_tier_policy), by relation OID. Not dumped: the objects belong to the physical relation.';
SELECT pg_catalog.pg_extension_config_dump('sift.tier_policies', 'WHERE false');

-- The retention of a sift_table: parts whose newest row is older than it are dropped. regclass is dumped by name and resolved again by name when the dump is restored.
CREATE TABLE IF NOT EXISTS sift.ttl_policies (
	relid		regclass PRIMARY KEY,
	col			text NOT NULL,
	retention	interval NOT NULL CHECK (retention > interval '0')
);
COMMENT ON TABLE sift.ttl_policies IS 'The TTL of each sift_table (set by sift.set_ttl), by relation OID.';
SELECT pg_catalog.pg_extension_config_dump('sift.ttl_policies', '');

-- A dropped relation takes its policies with it, so that an OID that PostgreSQL gives out again never meets a policy of the relation that had it. The objects of a dropped relation stay in the store
-- (a DROP can be rolled back, an object cannot): the notice names where, for the operator who deletes them.
CREATE OR REPLACE FUNCTION sift_internal.tier_policies_forget() RETURNS event_trigger
	LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog, pg_temp AS $fn$
DECLARE
	p record;
BEGIN
	FOR p IN
		DELETE FROM sift.tier_policies t
		USING pg_event_trigger_dropped_objects() d
		WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND t.relid::oid = d.objid
		RETURNING t.uri, t.scope_id
	LOOP
		RAISE NOTICE 'the objects of a dropped sift relation stay in %: directory r%', p.uri, lpad(to_hex(p.scope_id), 16, '0');
	END LOOP;
	DELETE FROM sift.ttl_policies t USING pg_event_trigger_dropped_objects() d
	WHERE d.classid = 'pg_class'::regclass AND d.objsubid = 0 AND t.relid::oid = d.objid;
END
$fn$;
DO $$ BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_event_trigger WHERE evtname = 'sift_tier_policies_forget') THEN
		CREATE EVENT TRIGGER sift_tier_policies_forget ON sql_drop EXECUTE FUNCTION sift_internal.tier_policies_forget();
	END IF;
END $$;
COMMENT ON EVENT TRIGGER sift_tier_policies_forget IS 'After DROP: removes the tier and TTL policies of the relations that were dropped';

-- Moves the parts of a sift_table or a sift_col index (Format 2.0) whose newest row is older than move_after to object storage at uri (file:///dir, s3://bucket/prefix or r2://bucket/prefix;
-- endpoint is required by r2:// and names a compatible store for s3://). The parts keep their place and read through a bounded local cache. true when the policy was set or changed, false when it was the same.
CREATE OR REPLACE FUNCTION sift.set_tier_policy(rel regclass, uri text, move_after interval DEFAULT '30 days', endpoint text DEFAULT NULL) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_set_tier_policy' LANGUAGE C VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift;
COMMENT ON FUNCTION sift.set_tier_policy(regclass, text, interval, text) IS 'Moves the parts of a Format 2.0 sift_table or sift_col index whose newest row is older than move_after to object storage (file:///dir, s3://bucket/prefix or r2://bucket/prefix with endpoint). Needs MAINTAIN. S3 and R2 read the AWS_* variables of the server environment and need libcurl.so.4. The location cannot change while parts are stored. true when the policy was set or changed.';

-- Stops moving parts. Refused while parts are still in object storage: they leave it only when they expire (a table) or when the index is rebuilt.
CREATE OR REPLACE FUNCTION sift.clear_tier_policy(rel regclass) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_clear_tier_policy' LANGUAGE C STRICT VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift;
COMMENT ON FUNCTION sift.clear_tier_policy(regclass) IS 'Removes the object storage policy of a sift_table or sift_col index: true when there was one. Refused while parts are in object storage. Needs MAINTAIN on the table.';

-- Drops the parts of a sift_table whose newest value of col is older than retention, in one commit per partition and without reading or deleting a row. A NULL retention clears the TTL.
-- A sift_col index is refused: it would forget rows its table keeps, and the same query would answer differently through the index and through the table.
CREATE OR REPLACE FUNCTION sift.set_ttl(rel regclass, col text, retention interval) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_set_ttl' LANGUAGE C VOLATILE SECURITY INVOKER SET search_path = pg_catalog, sift;
COMMENT ON FUNCTION sift.set_ttl(regclass, text, interval) IS 'Drops, in maintenance, the parts of a Format 2.0 sift_table whose newest value of col (a NOT NULL time column of the manifest) is older than retention: one commit per partition, no row read or deleted; a part with a row inside the retention stays whole. B-tree entries stay until REINDEX. A NULL retention clears the TTL. Needs MAINTAIN. Refused for a sift_col index.';

-- The parts in object storage of the relations the caller can read, one row each. The function is the view's source and runs as the caller, so that the privilege check is the caller's.
CREATE OR REPLACE FUNCTION sift_internal.tier_status()
RETURNS TABLE (relation regclass, part_id int8, part_gen int8, location text, byte_len int8, tiered_at timestamptz, expires_at timestamptz)
	AS 'MODULE_PATHNAME', 'sift_tier_status' LANGUAGE C STABLE SECURITY INVOKER SET search_path = pg_catalog, sift;
COMMENT ON FUNCTION sift_internal.tier_status() IS 'Internal: the rows of sift.tier_status';

-- Where each part in object storage is, how big, when it moved and, for a part that left the relation, when its object is deleted (expires_at). Shows no credential.
CREATE OR REPLACE VIEW sift.tier_status AS
	SELECT relation, part_id, part_gen, location, byte_len, tiered_at, expires_at FROM sift_internal.tier_status();
COMMENT ON VIEW sift.tier_status IS 'The parts in object storage of the sift_tables and sift_col indexes the caller has SELECT on, one row each: location (the object), size, when it moved and, for a part that left the relation, when its object is deleted (expires_at). Shows no credential.';
GRANT SELECT ON sift.tier_status TO PUBLIC;
