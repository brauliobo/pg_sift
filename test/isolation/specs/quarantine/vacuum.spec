# Damage found while a VACUUM runs. The VACUUM holds on the test gate before it reads the first segment of the index; meanwhile another session meets the damage and takes the index out
# of service. The VACUUM goes on without the index: it does not read a segment of it, it ends with a warning and no error, the dead rows are removed from the heap, and the statements
# that follow are planned on the heap and give its rows. REINDEX puts the index back.
# requires: col gates

setup
{
	SET sift.test_create_format1 = on;
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_qv (id int PRIMARY KEY, v int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_qv SELECT g, g % 7, 'x' FROM generate_series(1, 20000) g;
	CREATE INDEX iso_qv_idx ON iso_qv USING sift_col (id, v);
	DELETE FROM iso_qv WHERE id % 10 = 0;
	ANALYZE iso_qv;
	CREATE FUNCTION iso_qv_try(OUT result text) LANGUAGE plpgsql AS $$
	DECLARE n bigint;
	BEGIN
		PERFORM set_config('sift.on_corruption', 'quarantine', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		EXECUTE 'SELECT count(*) FROM iso_qv WHERE v = 3' INTO n;
		result := 'rows=' || n;
	EXCEPTION WHEN OTHERS THEN
		result := SQLSTATE;
	END $$;
	CREATE FUNCTION iso_qv_plan() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE r text; p text := '';
	BEGIN
		PERFORM set_config('sift.on_corruption', 'quarantine', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT count(*) FROM iso_qv WHERE v = 3' LOOP p := p || r; END LOOP;
		RETURN CASE WHEN p ~ 'Sift|iso_qv_idx' THEN 'sift' ELSE 'heap' END;
	END $$;
}

teardown
{
	DROP TABLE iso_qv;
	DROP FUNCTION iso_qv_try(), iso_qv_plan();
}

session s1
setup	{ SET sift.test_create_format1 = on; SET sift.on_corruption = quarantine; SET sift.test_bulkdelete_gate = 7102; }
step s1vacuum	{ VACUUM iso_qv; }
step s1rows	{ SELECT result FROM iso_qv_try(); }
step s1plan	{ SELECT iso_qv_plan() AS plan; }

session s2
setup	{ SET sift.test_create_format1 = on; SET sift.on_corruption = quarantine; }
step s2lock	{ SELECT pg_advisory_lock(7102); }
step s2poke	{ SELECT sift._test_poke('iso_qv_idx', 2, 8176, 9, 2); }
step s2meet	{ SELECT result FROM iso_qv_try(); }
step s2unlock	{ SELECT pg_advisory_unlock(7102); }
step s2listed	{ SELECT index, error_code, skipped_vacuums >= 1 AS vacuum_skipped FROM sift.quarantine; }
step s2heap	{ SELECT count(*) AS heap_rows FROM iso_qv WHERE v = 3; }
step s2reindex	{ REINDEX INDEX iso_qv_idx; }
step s2listed_after	{ SELECT count(*) AS listed FROM sift.quarantine; }

permutation s2lock s1vacuum s2poke s2meet s2unlock s2listed s1rows s2heap s1plan s2reindex s2listed_after s1rows s1plan
