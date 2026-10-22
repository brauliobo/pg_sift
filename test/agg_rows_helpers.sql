\set ECHO none
-- Shared by the agg tests that compare a query with and without the aggregate scan (\ir test/agg_rows_helpers.sql): agg_rows(q, push) is the sorted rows of q
-- as text with sift.enable_agg set to push; agg_pushed(q) tells whether the plan has a SiftAgg node.
CREATE FUNCTION agg_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION agg_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
\set ECHO all
