\set ECHO none
-- Shared by the agg_preagg* tests (\ir test/agg_preagg_helpers.sql). pa_rows(q, pre, push) is the sorted rows of q, or its error, with sift.agg_preagg and
-- sift.enable_agg set. A snapshot held by another session of this database (the refresh worker rewriting a segment, one that is still ending) keeps the
-- pages of rows written since from becoming all-visible, so the units stay scanned: pa_settle() waits for the conditions themselves, the queue drained
-- and no such session.
CREATE FUNCTION pa_rows(q text, pre bool, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.agg_preagg', pre::text, true);
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;
CREATE PROCEDURE pa_settle() LANGUAGE plpgsql AS $$
DECLARE deadline timestamptz := clock_timestamp() + interval '120 seconds';
BEGIN
	WHILE EXISTS (SELECT 1 FROM sift.col_rollup_queue())
	   OR EXISTS (SELECT 1 FROM pg_stat_activity WHERE datname = current_database() AND pid <> pg_backend_pid() AND backend_xmin IS NOT NULL) LOOP
		IF clock_timestamp() > deadline THEN
			RAISE EXCEPTION 'not settled: queue %, snapshots %', (SELECT count(*) FROM sift.col_rollup_queue()),
				(SELECT string_agg(backend_type || ' ' || backend_xmin, ', ') FROM pg_stat_activity
				 WHERE datname = current_database() AND pid <> pg_backend_pid() AND backend_xmin IS NOT NULL);
		END IF;
		PERFORM pg_sleep(0.01);
	END LOOP;
END $$;
\set ECHO all
