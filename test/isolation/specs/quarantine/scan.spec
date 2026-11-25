# Damage found while a scan runs. A scan holds on the test gate right after it opened the index; meanwhile another session meets the damage and takes the index out of service. The
# scan that goes on fails once with the damage error and its hint, the statement after it is planned on the heap and gives the rows of the heap. A scan that is cancelled
# (SQLSTATE 57014) leaves the index in service: nothing is recorded and the next statement is planned on the index. statement_timeout and lock_timeout at the same point are in test/t/quarantine_concurrent.sh.
# requires: col gates

setup
{
	CREATE EXTENSION IF NOT EXISTS pg_sift;
	CREATE EXTENSION IF NOT EXISTS pg_sift_test;
	CREATE TABLE iso_q (id int PRIMARY KEY, v int, pad text) WITH (autovacuum_enabled = off);
	INSERT INTO iso_q SELECT g, g % 7, 'x' FROM generate_series(1, 20000) g;
	CREATE INDEX iso_q_idx ON iso_q USING sift_col (id, v);
	ANALYZE iso_q;
}

setup
{
	CREATE FUNCTION iso_q_try(gate int, OUT result text, OUT hint text) LANGUAGE plpgsql AS $$
	DECLARE n bigint;
	BEGIN
		PERFORM set_config('sift.on_corruption', 'quarantine', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		PERFORM set_config('sift.test_col_open_gate', gate::text, true);
		EXECUTE 'SELECT count(*) FROM iso_q WHERE v = 3' INTO n;
		result := 'rows=' || n;
	EXCEPTION WHEN query_canceled OR OTHERS THEN
		GET STACKED DIAGNOSTICS hint = PG_EXCEPTION_HINT;
		result := SQLSTATE;
	END $$;
	CREATE FUNCTION iso_q_plan() RETURNS text LANGUAGE plpgsql AS $$
	DECLARE r text; p text := '';
	BEGIN
		PERFORM set_config('sift.on_corruption', 'quarantine', true);
		PERFORM set_config('max_parallel_workers_per_gather', '0', true);
		FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) SELECT count(*) FROM iso_q WHERE v = 3' LOOP p := p || r; END LOOP;
		RETURN CASE WHEN p ~ 'Sift|iso_q_idx' THEN 'sift' ELSE 'heap' END;
	END $$;
}

teardown
{
	DROP TABLE iso_q;
	DROP FUNCTION iso_q_try(int), iso_q_plan();
}

session s1
step s1scan	{ SELECT result, coalesce(hint ~ '^REINDEX INDEX', false) AS reindex_hint FROM iso_q_try(7101); }
step s1next	{ SELECT result, coalesce(hint ~ '^REINDEX INDEX', false) AS reindex_hint FROM iso_q_try(0); }
step s1plan	{ SELECT iso_q_plan() AS plan; }

session s2
setup	{ SET sift.on_corruption = quarantine; }
step s2lock	{ SELECT pg_advisory_lock(7101); }
step s2poke	{ SELECT sift._test_poke('iso_q_idx', 2, 8176, 9, 2); }
step s2meet	{ SELECT result, coalesce(hint ~ '^REINDEX INDEX', false) AS reindex_hint FROM iso_q_try(0); }
step s2unlock	{ SELECT pg_advisory_unlock(7101); }
step s2listed	{ SELECT index, error_code, detections FROM sift.quarantine; }
step s2none	{ SELECT count(*) AS listed FROM sift.quarantine; }
step s2cancel	{ SELECT pg_cancel_backend(pid) AS cancelled FROM pg_stat_activity WHERE query LIKE '%iso_q_try(7101)%' AND pid <> pg_backend_pid(); }

# the scan is held; another session meets the damage and quarantines the index; the scan goes on and meets it too
permutation s2lock s1scan s2poke s2meet s2unlock s2listed s1next s1plan
# the scan is held and cancelled
permutation s2lock s1scan s2cancel s2unlock s2none s1next s1plan
