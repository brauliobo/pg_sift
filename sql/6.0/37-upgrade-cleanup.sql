-- What the update from 5.x leaves behind in the catalog, and what the sweep after a disabled event trigger leaves.

-- The two helpers of the shared value store of a column are not implemented by the 6.0 library (src/core/retired.c only keeps their symbols for the frozen script of 5.1): the SQL functions that name
-- them would fail with "was removed" and nothing calls them, since sift.col_value_store_compact is an alias that does nothing
DO $$ BEGIN
	IF to_regprocedure('sift._col_value_store_begin(regclass, name)') IS NOT NULL THEN
		DROP FUNCTION sift._col_value_store_begin(regclass, name);
	END IF;
	IF to_regprocedure('sift._col_value_store_end(regclass)') IS NOT NULL THEN
		DROP FUNCTION sift._col_value_store_end(regclass);
	END IF;
END $$;

-- sift.ddl_hooks_enable(true) also removes the rows that the event triggers would have removed while they were disabled: the declared paths and options of tables that went, the unused value stores, and
-- the definitions of projections, the tier policies and the TTLs of relations that went (a projection definition with no OID is the row of a restore that waits for its index, and stays)
CREATE OR REPLACE FUNCTION sift.ddl_hooks_enable(enable boolean) RETURNS int LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $$
DECLARE
	n int := 0;
	h text;
	dropped record;
BEGIN
	FOR h IN SELECT name FROM sift.ddl_hooks LOOP
		EXECUTE format('ALTER EVENT TRIGGER %I %s', h, CASE WHEN enable THEN 'ENABLE' ELSE 'DISABLE' END);
		n := n + 1;
	END LOOP;
	IF enable THEN
		DELETE FROM sift.table_paths p WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = p.relid::oid);
		DELETE FROM sift.table_option_values v WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = v.rel::oid);
		DELETE FROM sift.projection_defs d WHERE d.rel::oid <> 0 AND NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = d.rel::oid);
		DELETE FROM sift.ttl_policies t WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = t.relid::oid);
		FOR dropped IN DELETE FROM sift.tier_policies t WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = t.relid::oid) RETURNING t.uri, t.scope_id
		LOOP
			RAISE NOTICE 'the objects of a dropped sift relation stay in %: directory r%', dropped.uri, lpad(to_hex(dropped.scope_id), 16, '0');
		END LOOP;
		PERFORM sift._col_value_store_drop_unused();
	END IF;
	RETURN n;
END $$;
COMMENT ON FUNCTION sift.ddl_hooks_enable(boolean) IS 'Superuser: disables (false) or enables (true) every event trigger of pg_sift in one call and returns how many it changed; enabling also removes what the drops left meanwhile (declared paths, options, projection definitions, tier and TTL policies, unused value stores). For a library that does not match the SQL, which fails every DDL statement a hook fires on.';
